/*
 * ExtBridge - ext2/3/4 for macOS
 * Public API of the portable ext4 core (libl2m).
 *
 * The core is plain C11 and has no macOS dependency. It is used by the
 * FSKit module on macOS and by the l2m command line tool and the test
 * suite on Linux. All functions return 0 / a positive count on success and
 * a negative errno value (-ENOENT, -EIO, ...) on failure.
 *
 * Thread safety: every public function takes the file system lock, so a
 * l2m_fs may be used from several threads. The lock is recursive: callbacks
 * may call read-only functions (l2m_getattr, l2m_readlink, ...) but must not
 * modify the volume.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef L2M_H
#define L2M_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define L2M_VERSION "0.9.3"

/* ---- block device abstraction ---------------------------------------- */

typedef struct l2m_dev {
	void *ctx;
	/* Read/write exactly len bytes at byte offset off. Return 0 or -errno. */
	int (*read)(void *ctx, uint64_t off, void *buf, size_t len);
	int (*write)(void *ctx, uint64_t off, const void *buf, size_t len);
	/* Make all previous writes durable (barrier). Return 0 or -errno. */
	int (*flush)(void *ctx);
	/* Optional: file data I/O. On macOS metadata goes through the kernel
	 * buffer cache while file data must bypass it; if NULL, read/write
	 * are used for both. */
	int (*read_data)(void *ctx, uint64_t off, void *buf, size_t len);
	int (*write_data)(void *ctx, uint64_t off, const void *buf, size_t len);
	uint64_t size;        /* bytes */
	uint32_t sector_size; /* physical sector size, usually 512 or 4096 */
} l2m_dev;

typedef struct l2m_fs l2m_fs;

/* mount flags */
#define L2M_MOUNT_RDONLY 0x01       /* never write to the device */
#define L2M_MOUNT_NO_ATIME 0x02     /* do not update access times */
#define L2M_MOUNT_FORCE_RO_REPLAY 0x04 /* replay journal in memory only */
#define L2M_MOUNT_SYNC 0x08         /* commit after every operation */

/* why a volume can only be mounted read-only (l2m_info.ro_reason) */
#define L2M_RO_NONE 0
#define L2M_RO_REQUESTED 1
#define L2M_RO_FEATURE 2     /* uses a feature we can only read */
#define L2M_RO_ERRORS 3      /* file system is marked as having errors */
#define L2M_RO_JOURNAL 4     /* journal needs recovery we cannot do safely */
#define L2M_RO_NOT_CLEAN 5   /* not cleanly unmounted (no journal) */

int l2m_mount(const l2m_dev *dev, unsigned flags, l2m_fs **out, char *errbuf, size_t errlen);
/* Flushes everything, marks the volume clean and frees fs. */
int l2m_umount(l2m_fs *fs);
/* Commits the running transaction and flushes the device. */
int l2m_sync(l2m_fs *fs);
/* Commits only if the transaction is older than max_age_ms (timer hook). */
int l2m_sync_if_older(l2m_fs *fs, unsigned max_age_ms);
int l2m_is_readonly(l2m_fs *fs);

/* ---- information / diagnostics ---------------------------------------- */

typedef struct l2m_info {
	char label[17];
	char uuid[37];
	char last_mounted[65];
	char features[512];      /* space separated feature names */
	char unsupported[256];   /* features preventing rw (or mount) */
	uint32_t block_size;
	uint64_t blocks_count;
	uint64_t free_blocks;
	uint64_t reserved_blocks;
	uint32_t inodes_count;
	uint32_t free_inodes;
	uint32_t inode_size;
	uint32_t groups;
	uint32_t blocks_per_group;
	uint32_t inodes_per_group;
	uint16_t state;          /* s_state */
	uint16_t errors_behaviour;
	uint16_t mount_count;
	int16_t max_mount_count;
	uint32_t error_count;
	int64_t mkfs_time, mount_time, write_time, lastcheck_time;
	int64_t first_error_time, last_error_time;
	uint64_t kbytes_written;
	uint32_t creator_os;
	uint32_t rev_level;
	int has_journal;
	uint32_t journal_blocks;
	uint32_t journal_features;
	int journal_needed_recovery; /* journal had to be replayed at mount */
	uint32_t journal_replayed_transactions;
	uint32_t journal_replayed_blocks;
	int metadata_csum;
	int readonly;
	int ro_reason;
	char hash_algo[16];
} l2m_info;

int l2m_get_info(l2m_fs *fs, l2m_info *info);
/* Set the volume label (max. 16 bytes, UTF-8). */
int l2m_set_label(l2m_fs *fs, const char *label);

/* Read-only consistency scan (checksums of superblock, group descriptors,
 * bitmaps, inode table sample, free counters). Calls progress(ctx, done,
 * total) now and then; returning non-zero from progress cancels.
 * Writes a human readable report into report (may be NULL). Returns the
 * number of problems found or -errno. */
typedef int (*l2m_progress_fn)(void *ctx, uint64_t done, uint64_t total);
int l2m_verify(l2m_fs *fs, l2m_progress_fn progress, void *ctx, char *report, size_t reportlen);
/* The same, with numbers the report only describes. Zero holes (see
 * l2m_zero_holes) are not counted as problems: they are legal, but in a
 * finished file they mean data that reads as zeros. */
typedef struct l2m_verify_stats {
	uint64_t zero_hole_files;
	uint64_t zero_hole_blocks;
	uint32_t zero_hole_inos[32]; /* the first ones */
} l2m_verify_stats;
int l2m_verify_ex(l2m_fs *fs, l2m_progress_fn progress, void *ctx, char *report, size_t reportlen, l2m_verify_stats *stats);
/* Walks the directory tree and calls fn with the path of each of the n
 * inodes it finds (hard links: every path). */
typedef void (*l2m_path_fn)(void *ctx, uint32_t ino, const char *path);
int l2m_find_paths(l2m_fs *fs, const uint32_t *inos, int n, l2m_path_fn fn, void *ctx);

/* Probe a device without mounting: 1 = ext2/3/4 found, 0 = no, <0 error.
 * Fills label/uuid if not NULL (17 / 37 bytes). */
int l2m_probe(const l2m_dev *dev, char *label, char *uuid, uint64_t *size_bytes);

/* ---- inodes / attributes ---------------------------------------------- */

typedef struct l2m_time {
	int64_t sec;
	uint32_t nsec;
} l2m_time;

typedef struct l2m_stat {
	uint32_t ino;
	uint32_t mode;     /* S_IFxxx | permission bits (Linux values) */
	uint32_t nlink;
	uint32_t uid, gid;
	uint64_t size;
	uint64_t alloc;    /* allocated bytes */
	uint32_t flags;    /* ext4 inode flags */
	uint32_t generation;
	uint32_t rdev;     /* device number for char/block devices */
	l2m_time atime, mtime, ctime, crtime;
} l2m_stat;

#define L2M_SET_MODE 0x01
#define L2M_SET_UID 0x02
#define L2M_SET_GID 0x04
#define L2M_SET_SIZE 0x08
#define L2M_SET_ATIME 0x10
#define L2M_SET_MTIME 0x20
#define L2M_SET_CRTIME 0x40
#define L2M_SET_FLAGS 0x80
#define L2M_SET_CTIME 0x100

int l2m_getattr(l2m_fs *fs, uint32_t ino, l2m_stat *st);
int l2m_setattr(l2m_fs *fs, uint32_t ino, unsigned mask, const l2m_stat *st);

/* ---- directories ------------------------------------------------------ */

#define L2M_ROOT_INO 2

/* d_type values (same as ext4 file type field) */
#define L2M_DT_UNKNOWN 0
#define L2M_DT_REG 1
#define L2M_DT_DIR 2
#define L2M_DT_CHR 3
#define L2M_DT_BLK 4
#define L2M_DT_FIFO 5
#define L2M_DT_SOCK 6
#define L2M_DT_LNK 7

int l2m_lookup(l2m_fs *fs, uint32_t dir, const char *name, size_t len, uint32_t *ino);
int l2m_lookup_path(l2m_fs *fs, const char *path, uint32_t *ino);

/* Callback returns 0 to continue, 1 to stop (entry not consumed), <0 error.
 * cookie: 0 = start; on return points behind the last consumed entry.
 * "." and ".." are reported too. */
typedef int (*l2m_dirent_fn)(void *ctx, const char *name, size_t len, uint32_t ino, unsigned dtype, uint64_t next_cookie);
int l2m_readdir(l2m_fs *fs, uint32_t dir, uint64_t *cookie, l2m_dirent_fn fn, void *ctx);
/* Changes whenever the directory changes (for cookie verifiers). */
uint64_t l2m_dir_version(l2m_fs *fs, uint32_t dir);

/* mode: S_IFREG/S_IFDIR/S_IFIFO/S_IFSOCK/S_IFCHR/S_IFBLK | perms */
int l2m_create(l2m_fs *fs, uint32_t dir, const char *name, size_t len, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t rdev, uint32_t *ino);
int l2m_symlink(l2m_fs *fs, uint32_t dir, const char *name, size_t len, const char *target, uint32_t uid, uint32_t gid, uint32_t *ino);
int l2m_readlink(l2m_fs *fs, uint32_t ino, char *buf, size_t buflen); /* returns length */
int l2m_link(l2m_fs *fs, uint32_t ino, uint32_t dir, const char *name, size_t len);
/* Removes a name. If the inode has no links left it stays allocated as an
 * orphan until l2m_release() is called (open-unlinked semantics); *orphan
 * tells the caller. Directories must be empty. */
int l2m_unlink(l2m_fs *fs, uint32_t dir, const char *name, size_t len, int is_dir, int *orphan);
#define L2M_RENAME_NOREPLACE 1
#define L2M_RENAME_EXCHANGE 2
int l2m_rename(l2m_fs *fs, uint32_t sdir, const char *sname, size_t slen, uint32_t ddir, const char *dname, size_t dlen, unsigned flags, uint32_t *replaced_ino, int *replaced_orphan);
/* Called when the last user of an inode is gone: frees it if nlink == 0. */
int l2m_release(l2m_fs *fs, uint32_t ino);

/* ---- file data -------------------------------------------------------- */

int64_t l2m_read(l2m_fs *fs, uint32_t ino, uint64_t off, void *buf, size_t len);
int64_t l2m_write(l2m_fs *fs, uint32_t ino, uint64_t off, const void *buf, size_t len);
int l2m_truncate(l2m_fs *fs, uint32_t ino, uint64_t size);
/* Allocate (as unwritten extents) without changing the size. */
int l2m_preallocate(l2m_fs *fs, uint32_t ino, uint64_t off, uint64_t len, uint64_t *allocated);

/* Physical mapping for kernel offloaded I/O. */
#define L2M_EXT_HOLE 0
#define L2M_EXT_DATA 1
#define L2M_EXT_UNWRITTEN 2 /* allocated, reads as zeros */
typedef struct l2m_extent {
	uint64_t lblk;   /* first logical block */
	uint64_t pblk;   /* first physical block (0 for holes) */
	uint32_t len;    /* number of blocks */
	uint32_t type;
} l2m_extent;
#define L2M_MAP_ALLOCATE 0x1 /* fill holes with new unwritten extents */
/* Returns the number of extents written into out (covering at least the
 * first block of the range, at most nmax). */
int l2m_map(l2m_fs *fs, uint32_t ino, uint64_t lblk, uint64_t count, unsigned flags, l2m_extent *out, int nmax);
/* After the kernel wrote [off, off+len): convert unwritten extents in the
 * range to written and grow i_size to at least new_size. */
int l2m_write_done(l2m_fs *fs, uint32_t ino, uint64_t off, uint64_t len, uint64_t new_size);

/* Kernel-offloaded I/O as macOS does it (FSKit): the kernel maps ranges for
 * writing within numbered operations, writes the data itself and reports
 * each operation done once, with a single byte range that only summarises
 * it. These calls remember which unwritten blocks were handed out to which
 * operation and convert exactly those (see ops.c):
 *   l2m_koio_map      like l2m_map; with L2M_MAP_ALLOCATE the unwritten
 *                     extents returned are handed out to operation op
 *   l2m_koio_unmap    take back what op got of [lblk, lblk+count) (the
 *                     caller could not pass it on to the kernel)
 *   l2m_koio_done     operation op finished; ok = 0 if it failed. Grows the
 *                     file to off+len.
 *   l2m_koio_release  the kernel is finished with the file (ino 0: with all
 *                     files, before unmounting) */
int l2m_koio_map(l2m_fs *fs, uint32_t ino, uint64_t op, uint64_t lblk, uint64_t count, unsigned flags, l2m_extent *out, int nmax);
int l2m_koio_unmap(l2m_fs *fs, uint32_t ino, uint64_t op, uint64_t lblk, uint64_t count);
int l2m_koio_done(l2m_fs *fs, uint32_t ino, uint64_t op, uint64_t off, uint64_t len, int ok);
int l2m_koio_release(l2m_fs *fs, uint32_t ino);
/* "Zero holes": unwritten extents inside the file size. They are legal
 * (fallocate leaves them, and so does a copy that is still running), but
 * in a finished file they read as zeros where data was meant to be. Calls
 * fn (if not NULL) for each one, clipped to the size; a nonzero return
 * stops. Returns the number of such blocks, or -errno. */
typedef int (*l2m_zero_fn)(void *ctx, uint64_t lblk, uint64_t pblk, uint32_t len);
int64_t l2m_zero_holes(l2m_fs *fs, uint32_t ino, l2m_zero_fn fn, void *ctx);

/* ---- extended attributes ---------------------------------------------- */
/* Names are in macOS form: "com.apple.FinderInfo" is stored as Linux
 * "user.com.apple.FinderInfo". Linux names in other namespaces (security.,
 * trusted., system.) are reported with their prefix and are read-only. */
int64_t l2m_getxattr(l2m_fs *fs, uint32_t ino, const char *name, void *buf, size_t len);
#define L2M_XATTR_CREATE 1
#define L2M_XATTR_REPLACE 2
int l2m_setxattr(l2m_fs *fs, uint32_t ino, const char *name, const void *val, size_t len, unsigned flags);
int l2m_removexattr(l2m_fs *fs, uint32_t ino, const char *name);
/* NUL separated list, returns total length. */
int64_t l2m_listxattr(l2m_fs *fs, uint32_t ino, char *buf, size_t len);

/* ---- statistics -------------------------------------------------------- */
typedef struct l2m_statfs {
	uint32_t block_size;
	uint64_t blocks, bfree, bavail;
	uint64_t files, ffree;
	uint32_t namemax;
} l2m_statfs;
int l2m_statfs_get(l2m_fs *fs, l2m_statfs *st);

/* For tools: a device backed by a file descriptor (regular file or raw
 * device). Returns 0 or -errno. */
int l2m_dev_open_fd(int fd, int writable, l2m_dev *dev);
void l2m_dev_close_fd(l2m_dev *dev);

/* Last diagnostic message (per fs), useful for logs. */
const char *l2m_last_error(l2m_fs *fs);

/* Optional log sink (default: none). */
typedef void (*l2m_log_fn)(int level, const char *msg);
void l2m_set_log(l2m_log_fn fn);

#ifdef __cplusplus
}
#endif
#endif
