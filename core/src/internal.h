/*
 * ExtBridge - internal declarations of the ext4 core.
 *
 * Locking: fs->lock is taken by every public API function (api.c). All
 * functions declared here expect the lock to be held.
 *
 * Write model (see docs/ARCHITECTURE.md):
 *  - Every metadata change goes through the block cache. Modified buffers
 *    are attached to the running transaction (fs->txn).
 *  - A transaction is committed at the end of an operation when it is old
 *    or big enough, on sync and on unmount. Commit = write all dirty
 *    buffers to the jbd2 journal, write the commit block, then write them
 *    to their home locations ("checkpoint") and mark the journal empty.
 *    A crash at any point leaves either the old or the new state.
 *  - Blocks freed in the running transaction are not reused before it is
 *    committed (fs->txn.freed), so a crash can never expose new data in a
 *    block that the old, still valid metadata points to.
 *  - File data is written directly to its final location before the
 *    metadata that references it is committed (ordered mode).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef L2M_INTERNAL_H
#define L2M_INTERNAL_H

#include "ext4_disk.h"
#include "l2m/l2m.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef EOPNOTSUPP
#define EOPNOTSUPP ENOTSUP
#endif
#ifndef EUCLEAN
#define EUCLEAN 117 /* "structure needs cleaning" (corruption) */
#endif
#define EFSCORRUPTED EUCLEAN
#define EFSBADCRC EBADMSG

#define L2M_MIN(a, b) ((a) < (b) ? (a) : (b))
#define L2M_MAX(a, b) ((a) > (b) ? (a) : (b))
#define L2M_DIV_ROUND_UP(a, b) (((a) + (b) - 1) / (b))

/* ---- logging ---------------------------------------------------------- */
#define L2M_LOG_ERROR 0
#define L2M_LOG_WARN 1
#define L2M_LOG_INFO 2
#define L2M_LOG_DEBUG 3
void l2m_log(l2m_fs *fs, int level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
/* Report a corruption: logs, remembers it and returns -EFSCORRUPTED. */
int l2m_corrupt(l2m_fs *fs, const char *what, uint64_t where);

/* ---- checksums / hashes ----------------------------------------------- */
static inline int getenv_flag_no_hw_crc(void)
{
	static int v = -1;
	if (v < 0) {
		const char *e = getenv("L2M_NO_HW_CRC");
		v = (e && *e == '1') ? 1 : 0;
	}
	return v;
}
uint32_t l2m_crc32c(uint32_t crc, const void *buf, size_t len);
uint32_t l2m_crc32c_sw(uint32_t crc, const void *buf, size_t len);
uint16_t l2m_crc16(uint16_t crc, const void *buf, size_t len);
int l2m_dirhash(int version, const uint32_t seed[4], const char *name, int len, uint32_t *hash, uint32_t *minor);

/* ---- block cache ------------------------------------------------------ */
struct l2m_buf {
	uint64_t blk;
	uint8_t *data;
	struct l2m_buf *hnext;
	struct l2m_buf *lprev, *lnext; /* LRU of clean, unreferenced buffers */
	struct l2m_buf *dnext;         /* transaction dirty list */
	uint32_t refs;
	uint8_t dirty;
	uint8_t pinned;  /* never evicted (superblock, group descriptors) */
	uint8_t overlay; /* read-only journal replay result, never written */
	uint8_t in_lru;
};

struct l2m_cache {
	struct l2m_buf **hash;
	uint32_t hash_size;
	struct l2m_buf *lru_head, *lru_tail; /* head = most recent */
	uint32_t count, clean_max;
	uint64_t hits, misses;
};

int buf_get(l2m_fs *fs, uint64_t blk, struct l2m_buf **out);    /* read */
int buf_get_new(l2m_fs *fs, uint64_t blk, struct l2m_buf **out); /* zeroed, no read */
void buf_put(l2m_fs *fs, struct l2m_buf *b);
int buf_dirty(l2m_fs *fs, struct l2m_buf *b); /* attach to transaction */
/* Drop a block that was freed (it must not be written back later). */
void buf_forget(l2m_fs *fs, uint64_t blk);
void cache_destroy(l2m_fs *fs);
int cache_init(l2m_fs *fs);
void cache_invalidate_clean(l2m_fs *fs);
int cache_peek(l2m_fs *fs, uint64_t blk, struct l2m_buf **out); /* no ref taken */
static inline bool uuid_is_null_bytes(const uint8_t *u) { for (int i = 0; i < 16; i++) if (u[i]) return false; return true; }

int dev_read_blocks(l2m_fs *fs, uint64_t blk, uint32_t count, void *buf);
int dev_write_blocks(l2m_fs *fs, uint64_t blk, uint32_t count, const void *buf);
int dev_flush(l2m_fs *fs);
int dev_read_data(l2m_fs *fs, uint64_t blk, uint32_t count, void *buf);
int dev_write_data(l2m_fs *fs, uint64_t blk, uint32_t count, const void *buf);

/* ---- transactions / journal ------------------------------------------ */
struct l2m_range {
	uint64_t start;
	uint64_t len;
};

struct l2m_txn {
	struct l2m_buf *dirty_head;
	uint32_t ndirty;
	uint64_t start_ms; /* 0 = no transaction running */
	struct l2m_range *freed;
	uint32_t nfreed, freed_cap;
	uint64_t freed_blocks; /* sum of freed (not yet reusable) blocks */
	uint32_t ops;
};

struct l2m_jmap {
	uint64_t lblk;
	uint64_t pblk;
	uint32_t len;
};

struct l2m_journal {
	int present;
	uint32_t bs;
	uint32_t first, maxlen;
	uint32_t sequence; /* id of the next transaction */
	uint32_t incompat, compat;
	uint32_t csum_seed;
	int csum_v2, csum_v3, is64;
	uint32_t tag_bytes;
	struct l2m_jmap *map;
	uint32_t nmap;
	uint8_t *sbbuf;
	/* the "journal empty" superblock was written but not flushed yet: it
	 * must be durable before the log area is reused (like jbd2's tail
	 * update with FUA) */
	int sb_unflushed;
	/* recovery results */
	int needed_recovery;
	uint32_t replayed_trans, replayed_blocks;
	int recovery_errors;
};

int txn_begin(l2m_fs *fs);
int txn_commit(l2m_fs *fs);         /* commit + checkpoint now */
int txn_end_op(l2m_fs *fs);         /* end of an operation: commit if due */
int txn_need_room(l2m_fs *fs, uint32_t blocks); /* commit if not enough room */
/* -ENOSPC while freed blocks wait for the commit: commit so they become
 * usable (caller must be at a consistent point). 1 = retry makes sense. */
int txn_enospc_retry(l2m_fs *fs);
int txn_add_freed(l2m_fs *fs, uint64_t start, uint64_t len);
bool txn_is_freed(l2m_fs *fs, uint64_t blk, uint64_t *next_ok);
uint32_t txn_max_blocks(l2m_fs *fs);

int journal_load(l2m_fs *fs);
int journal_recover(l2m_fs *fs, int to_memory);
int journal_write_txn(l2m_fs *fs, struct l2m_buf **bufs, uint32_t n);
int journal_mark_empty(l2m_fs *fs);
void journal_free(l2m_fs *fs);

/* ---- inodes ------------------------------------------------------------ */
#define L2M_MAX_INODE_SIZE 1024
struct l2m_inode {
	uint32_t ino;
	uint32_t csum_seed;
	union {
		struct ext4_inode i;
		uint8_t raw[L2M_MAX_INODE_SIZE];
	};
};

int inode_read(l2m_fs *fs, uint32_t ino, struct l2m_inode *in);
int inode_write(l2m_fs *fs, struct l2m_inode *in);
uint64_t inode_size(const struct l2m_inode *in);
void inode_set_size(struct l2m_inode *in, uint64_t size);
uint64_t inode_blocks512(l2m_fs *fs, const struct l2m_inode *in);
void inode_add_blocks(l2m_fs *fs, struct l2m_inode *in, int64_t fs_blocks);
uint32_t inode_uid(const struct l2m_inode *in);
uint32_t inode_gid(const struct l2m_inode *in);
uint64_t inode_file_acl(const struct l2m_inode *in);
void inode_set_file_acl(struct l2m_inode *in, uint64_t blk);
void inode_get_time(const struct l2m_inode *in, int which, l2m_time *t);
void inode_set_time(l2m_fs *fs, struct l2m_inode *in, int which, const l2m_time *t);
void inode_touch(l2m_fs *fs, struct l2m_inode *in, int which_mask);
void l2m_now(l2m_time *t);
uint32_t inode_compute_csum(l2m_fs *fs, struct l2m_inode *in);
void inode_init_seed(l2m_fs *fs, struct l2m_inode *in);
bool inode_has_extra(l2m_fs *fs, const struct l2m_inode *in, size_t field_end);
#define T_ATIME 1
#define T_MTIME 2
#define T_CTIME 4
#define T_CRTIME 8
static inline uint32_t inode_mode(const struct l2m_inode *in) { return in->i.i_mode; }
static inline bool inode_is_dir(const struct l2m_inode *in) { return (in->i.i_mode & L2M_S_IFMT) == L2M_S_IFDIR; }
static inline bool inode_is_reg(const struct l2m_inode *in) { return (in->i.i_mode & L2M_S_IFMT) == L2M_S_IFREG; }
static inline bool inode_is_lnk(const struct l2m_inode *in) { return (in->i.i_mode & L2M_S_IFMT) == L2M_S_IFLNK; }
bool inode_is_fast_symlink(l2m_fs *fs, const struct l2m_inode *in);
/* Location of an inode in the inode table. */
int inode_location(l2m_fs *fs, uint32_t ino, uint64_t *blk, uint32_t *off);

/* ---- block mapping (extents + legacy indirect) ------------------------ */
struct l2m_map {
	uint64_t lblk;
	uint64_t pblk; /* 0 = hole */
	uint32_t len;
	bool unwritten;
};
/* Map lblk; returns the extent/hole containing lblk (len >= 1). For holes
 * len is the distance to the next mapped block (capped). */
int bmap(l2m_fs *fs, struct l2m_inode *in, uint64_t lblk, struct l2m_map *m);
/* Allocate blocks for [lblk, lblk+count) holes; returns mapping of the
 * first piece (may be shorter). unwritten: mark new extents unwritten. */
int bmap_alloc(l2m_fs *fs, struct l2m_inode *in, uint64_t lblk, uint64_t count, bool unwritten, uint64_t goal, struct l2m_map *m);
/* Free all blocks from lblk on (truncate). */
int bmap_truncate(l2m_fs *fs, struct l2m_inode *in, uint64_t from_lblk);
/* Mark [lblk, lblk+count) as written (converts unwritten extents). */
int ext_mark_written(l2m_fs *fs, struct l2m_inode *in, uint64_t lblk, uint64_t count);
int ext_init_root(struct l2m_inode *in);
int ext_verify_tree(l2m_fs *fs, struct l2m_inode *in, uint64_t *nblocks); /* consistency check */
/* Calls fn for every block of an extent tree: nodes (meta, len 1) and data extents. */
typedef int (*ext_walk_fn)(void *ctx, uint64_t pblk, uint64_t len, bool meta);
int ext_walk(l2m_fs *fs, struct l2m_inode *in, ext_walk_fn fn, void *ctx);
void ext_describe(l2m_fs *fs, struct l2m_inode *in, void (*out)(void *ctx, const char *line), void *ctx);
uint32_t ext_tail_csum(l2m_fs *fs, struct l2m_inode *in, const uint8_t *block);

/* ---- allocation --------------------------------------------------------- */
int alloc_blocks(l2m_fs *fs, uint64_t goal, uint32_t want, uint64_t *start, uint32_t *got, bool metadata);
int free_blocks(l2m_fs *fs, uint64_t start, uint64_t count);
int alloc_inode(l2m_fs *fs, uint32_t parent, bool is_dir, uint32_t *ino);
int free_inode(l2m_fs *fs, uint32_t ino, bool is_dir);
bool inode_allocated(l2m_fs *fs, uint32_t ino, int *err);
bool block_in_use(l2m_fs *fs, uint64_t blk, int *err);
uint64_t goal_for_inode(l2m_fs *fs, uint32_t ino);

/* ---- group descriptors ------------------------------------------------ */
struct ext4_group_desc *gd_get(l2m_fs *fs, uint32_t group);
int gd_dirty(l2m_fs *fs, uint32_t group);
uint64_t gd_block_bitmap(l2m_fs *fs, const struct ext4_group_desc *gd);
uint64_t gd_inode_bitmap(l2m_fs *fs, const struct ext4_group_desc *gd);
uint64_t gd_inode_table(l2m_fs *fs, const struct ext4_group_desc *gd);
uint32_t gd_free_blocks(l2m_fs *fs, const struct ext4_group_desc *gd);
uint32_t gd_free_inodes(l2m_fs *fs, const struct ext4_group_desc *gd);
uint32_t gd_used_dirs(l2m_fs *fs, const struct ext4_group_desc *gd);
uint32_t gd_itable_unused(l2m_fs *fs, const struct ext4_group_desc *gd);
void gd_set_free_blocks(l2m_fs *fs, struct ext4_group_desc *gd, uint32_t v);
void gd_set_free_inodes(l2m_fs *fs, struct ext4_group_desc *gd, uint32_t v);
void gd_set_used_dirs(l2m_fs *fs, struct ext4_group_desc *gd, uint32_t v);
void gd_set_itable_unused(l2m_fs *fs, struct ext4_group_desc *gd, uint32_t v);
uint16_t gd_csum(l2m_fs *fs, uint32_t group, const struct ext4_group_desc *gd);
uint32_t bitmap_csum(l2m_fs *fs, const uint8_t *bitmap, uint32_t bytes);
int group_has_super(l2m_fs *fs, uint32_t group);
uint32_t group_super_overhead(l2m_fs *fs, uint32_t group); /* superblock + descriptors at the group start */
uint64_t group_first_block(l2m_fs *fs, uint32_t group);
uint32_t group_nblocks(l2m_fs *fs, uint32_t group);
/* Reads the block bitmap, initialising BLOCK_UNINIT groups in memory. */
int block_bitmap_get(l2m_fs *fs, uint32_t group, struct l2m_buf **out, bool for_write);
int inode_bitmap_get(l2m_fs *fs, uint32_t group, struct l2m_buf **out, bool for_write);
int block_bitmap_update_csum(l2m_fs *fs, uint32_t group, struct l2m_buf *b);
int inode_bitmap_update_csum(l2m_fs *fs, uint32_t group, struct l2m_buf *b);
int sb_dirty(l2m_fs *fs);
uint64_t sb_free_blocks(l2m_fs *fs);
void sb_add_free_blocks(l2m_fs *fs, int64_t delta);
void sb_add_free_inodes(l2m_fs *fs, int64_t delta);

/* ---- directories ------------------------------------------------------- */
int dir_lookup(l2m_fs *fs, struct l2m_inode *dir, const char *name, size_t len, uint32_t *ino, uint8_t *ftype);
int dir_add(l2m_fs *fs, struct l2m_inode *dir, const char *name, size_t len, uint32_t ino, uint8_t ftype);
int dir_remove(l2m_fs *fs, struct l2m_inode *dir, const char *name, size_t len, uint32_t *ino);
int dir_set_entry(l2m_fs *fs, struct l2m_inode *dir, const char *name, size_t len, uint32_t ino, uint8_t ftype);
int dir_is_empty(l2m_fs *fs, struct l2m_inode *dir);
int dir_init(l2m_fs *fs, struct l2m_inode *dir, uint32_t parent);
int dir_iterate(l2m_fs *fs, struct l2m_inode *dir, uint64_t *cookie, l2m_dirent_fn fn, void *ctx);
int dir_check_block(l2m_fs *fs, struct l2m_inode *dir, const uint8_t *block, uint64_t lblk, bool *is_dx);
uint8_t mode_to_ftype(uint32_t mode);
int dir_read_block(l2m_fs *fs, struct l2m_inode *dir, uint64_t lblk, struct l2m_buf **out);

/* ---- extended attributes ------------------------------------------------ */
int xattr_get(l2m_fs *fs, struct l2m_inode *in, int index, const char *name, void *buf, size_t len);
int xattr_set(l2m_fs *fs, struct l2m_inode *in, int index, const char *name, const void *val, size_t len, unsigned flags);
int64_t xattr_list(l2m_fs *fs, struct l2m_inode *in, char *buf, size_t len);
int xattr_free_all(l2m_fs *fs, struct l2m_inode *in);

/* ---- orphans ----------------------------------------------------------- */
int orphan_add(l2m_fs *fs, struct l2m_inode *in);
int orphan_del(l2m_fs *fs, struct l2m_inode *in);
int orphan_cleanup(l2m_fs *fs);

/* ---- file system ------------------------------------------------------- */
struct l2m_fs {
	l2m_dev dev;
	pthread_mutex_t lock;
	unsigned flags;
	int rdonly;
	int ro_reason;
	int verify_csums;

	struct l2m_buf *sbbuf;           /* buffer holding the primary superblock */
	struct ext4_super_block *sb;     /* points into sbbuf */
	struct ext4_super_block sb_orig; /* as read at mount (diagnostics) */

	uint32_t bs, log_bs;
	uint64_t blocks_count;
	uint32_t first_data_block;
	uint32_t groups, ipg, bpg, inode_size, desc_size, desc_per_block;
	uint32_t first_ino;
	uint32_t gdt_blocks, inode_table_blocks;
	struct l2m_buf **gdt;            /* pinned descriptor blocks */
	uint32_t ngdt;
	uint32_t csum_seed;
	bool has_csum, has_gdt_csum, is64, has_extents, has_flex, has_dir_index;
	bool has_filetype, has_huge_file, has_dir_nlink, has_extra_isize, has_largedir;
	bool has_meta_bg, has_journal, has_xattr, has_inline_data, has_sparse_super2;
	uint32_t hash_seed[4];
	int hash_unsigned;
	int def_hash;
	uint16_t want_extra_isize;

	struct l2m_cache cache;
	struct l2m_txn txn;
	struct l2m_journal journal;
	uint32_t commit_interval_ms;
	uint32_t txn_limit;      /* commit when a transaction has this many blocks */

	uint64_t dir_gen;        /* bumped on every namespace change */
	uint32_t last_alloc_group;
	uint8_t *zero_block;

	char last_error[512];
	uint32_t errors_seen;
	char unsupported[256];
	uint64_t bytes_read, bytes_written;
};

static inline bool fs_csum(const l2m_fs *fs) { return fs->has_csum; }
static inline uint32_t le32_at(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint16_t le16_at(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }

uint64_t l2m_now_ms(void);
int fs_check_writable(l2m_fs *fs);

#endif
