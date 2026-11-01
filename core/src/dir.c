/*
 * ExtBridge - directories: linear and hashed (htree / dir_index).
 *
 * Reading walks all directory blocks linearly (htree index blocks look
 * like empty entries and are skipped), so readdir works for every layout.
 * Lookups in indexed directories use the hash tree. Adding entries keeps
 * the hash tree valid: leaves are split by hash, index nodes are split or a
 * level is added just like Linux does it, and a one-block directory that
 * runs full is converted to an indexed one.
 *
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

#define DOT_REC 12u
#define DX_ROOT_OFF 32u /* "." (12) + ".." header (12) + dx_root_info (8) */
#define DX_NODE_OFF 8u
#define TAIL 12u

static inline struct ext4_dir_entry_2 *de_at(uint8_t *b, uint32_t off) { return (struct ext4_dir_entry_2 *)(b + off); }

uint8_t mode_to_ftype(uint32_t mode)
{
	switch (mode & L2M_S_IFMT) {
	case L2M_S_IFREG: return EXT4_FT_REG_FILE;
	case L2M_S_IFDIR: return EXT4_FT_DIR;
	case L2M_S_IFCHR: return EXT4_FT_CHRDEV;
	case L2M_S_IFBLK: return EXT4_FT_BLKDEV;
	case L2M_S_IFIFO: return EXT4_FT_FIFO;
	case L2M_S_IFSOCK: return EXT4_FT_SOCK;
	case L2M_S_IFLNK: return EXT4_FT_SYMLINK;
	}
	return EXT4_FT_UNKNOWN;
}

static uint32_t leaf_end(l2m_fs *fs) { return fs->bs - (fs->has_csum ? TAIL : 0); }

static uint32_t dirent_csum(l2m_fs *fs, struct l2m_inode *dir, const uint8_t *b)
{
	return l2m_crc32c(dir->csum_seed, b, fs->bs - TAIL);
}

static void leaf_set_tail(l2m_fs *fs, struct l2m_inode *dir, uint8_t *b)
{
	if (!fs->has_csum)
		return;
	struct ext4_dir_entry_tail *t = (void *)(b + fs->bs - TAIL);
	memset(t, 0, sizeof(*t));
	t->det_rec_len = TAIL;
	t->det_reserved_ft = EXT4_FT_DIR_CSUM;
	t->det_checksum = dirent_csum(fs, dir, b);
}

static bool has_tail(l2m_fs *fs, const uint8_t *b)
{
	const struct ext4_dir_entry_tail *t = (const void *)(b + fs->bs - TAIL);
	return t->det_reserved_zero1 == 0 && t->det_rec_len == TAIL && t->det_reserved_zero2 == 0 && t->det_reserved_ft == EXT4_FT_DIR_CSUM;
}

/* ---- htree helpers --------------------------------------------------------- */

static uint32_t dx_limit(l2m_fs *fs, uint32_t off) { return (fs->bs - off - (fs->has_csum ? sizeof(struct dx_tail) : 0)) / sizeof(struct dx_entry); }

static uint32_t dx_csum(l2m_fs *fs, struct l2m_inode *dir, const uint8_t *b, uint32_t off)
{
	const struct dx_countlimit *cl = (const void *)(b + off);
	uint32_t size = off + cl->count * (uint32_t)sizeof(struct dx_entry);
	const struct dx_tail *t = (const void *)(b + off + cl->limit * sizeof(struct dx_entry));
	uint32_t dummy = 0;
	uint32_t c = l2m_crc32c(dir->csum_seed, b, size);
	c = l2m_crc32c(c, t, offsetof(struct dx_tail, dt_checksum));
	return l2m_crc32c(c, &dummy, sizeof(dummy)); /* the checksum field itself counts as zero */
}

static void dx_set_csum(l2m_fs *fs, struct l2m_inode *dir, uint8_t *b, uint32_t off)
{
	if (!fs->has_csum)
		return;
	struct dx_countlimit *cl = (void *)(b + off);
	struct dx_tail *t = (void *)(b + off + cl->limit * sizeof(struct dx_entry));
	t->dt_reserved = 0;
	t->dt_checksum = dx_csum(fs, dir, b, off);
}

static bool is_dx_node(l2m_fs *fs, const uint8_t *b)
{
	const struct ext4_dir_entry_2 *d = (const void *)b;
	return d->inode == 0 && d->rec_len == fs->bs && d->name_len == 0;
}

int dir_read_block(l2m_fs *fs, struct l2m_inode *dir, uint64_t lblk, struct l2m_buf **out)
{
	struct l2m_map m;
	int r = bmap(fs, dir, lblk, &m);
	if (r)
		return r;
	if (!m.pblk || m.unwritten)
		return -ENODATA; /* hole in a directory */
	return buf_get(fs, m.pblk, out);
}

/* Validate a directory block. is_dx: block is an htree index block. */
int dir_check_block(l2m_fs *fs, struct l2m_inode *dir, const uint8_t *b, uint64_t lblk, bool *is_dx)
{
	bool indexed = (dir->i.i_flags & EXT4_INDEX_FL) && fs->has_dir_index;
	*is_dx = false;
	if (indexed && (lblk == 0 || is_dx_node(fs, b))) {
		uint32_t off = lblk == 0 ? DX_ROOT_OFF : DX_NODE_OFF;
		const struct dx_countlimit *cl = (const void *)(b + off);
		if (lblk == 0) {
			const struct dx_root_info *info = (const void *)(b + 24);
			const struct ext4_dir_entry_2 *dot = (const void *)b;
			if (dot->rec_len != DOT_REC || info->reserved_zero || info->info_length != 8)
				goto linear; /* not a dx root after all (Linux falls back too) */
		}
		if (cl->limit != dx_limit(fs, off) || cl->count > cl->limit || cl->count == 0)
			goto linear;
		*is_dx = true;
		if (fs->has_csum && fs->verify_csums) {
			const struct dx_tail *t = (const void *)(b + off + cl->limit * sizeof(struct dx_entry));
			if (t->dt_checksum != dx_csum(fs, dir, b, off)) {
				l2m_log(fs, L2M_LOG_ERROR, "directory %u: htree block %llu checksum mismatch", dir->ino, (unsigned long long)lblk);
				return -EFSBADCRC;
			}
		}
		return 0;
	}
linear:;
	uint32_t end = fs->bs;
	if (fs->has_csum) {
		if (has_tail(fs, b)) {
			end = fs->bs - TAIL;
			if (fs->verify_csums && le32_at(b + fs->bs - 4) != dirent_csum(fs, dir, b)) {
				l2m_log(fs, L2M_LOG_ERROR, "directory %u: block %llu checksum mismatch", dir->ino, (unsigned long long)lblk);
				return -EFSBADCRC;
			}
		} else if (fs->verify_csums) {
			l2m_log(fs, L2M_LOG_ERROR, "directory %u: block %llu has no checksum tail", dir->ino, (unsigned long long)lblk);
			return -EFSBADCRC;
		}
	}
	for (uint32_t off = 0; off < end;) {
		const struct ext4_dir_entry_2 *d = (const void *)(b + off);
		if (off + 8 > end || d->rec_len < 12 || (d->rec_len & 3) || off + d->rec_len > end ||
		    (uint32_t)d->name_len + 8 > d->rec_len || d->inode > fs->sb->s_inodes_count)
			return l2m_corrupt(fs, "bad directory entry in directory", dir->ino);
		off += d->rec_len;
	}
	return 0;
}

static int hash_version(l2m_fs *fs, int v)
{
	if (v <= EXT4_HASH_TEA)
		v += fs->hash_unsigned;
	return v;
}

static int name_hash(l2m_fs *fs, int version, const char *name, size_t len, uint32_t *hash, uint32_t *minor)
{
	return l2m_dirhash(hash_version(fs, version), fs->hash_seed, name, (int)len, hash, minor);
}

struct dx_frame {
	struct l2m_buf *buf;
	uint32_t off;        /* offset of dx_countlimit */
	struct dx_entry *entries;
	uint32_t at;         /* index of entry taken */
};

static void frames_release(l2m_fs *fs, struct dx_frame *f, int n)
{
	for (int i = 0; i < n; i++)
		if (f[i].buf)
			buf_put(fs, f[i].buf), f[i].buf = NULL;
}

static int max_levels(l2m_fs *fs) { return fs->has_largedir ? 3 : 2; }

/* Walk the index for hash. Returns number of frames (levels+1) or -err.
 * -EAGAIN: the index is unusable, fall back to linear scan. */
static int dx_probe(l2m_fs *fs, struct l2m_inode *dir, uint32_t hash, struct dx_frame *fr, int *version)
{
	struct l2m_buf *b;
	bool dx;
	memset(fr, 0, sizeof(*fr) * 3);
	int r = dir_read_block(fs, dir, 0, &b);
	if (r)
		return r;
	r = dir_check_block(fs, dir, b->data, 0, &dx);
	if (r || !dx) {
		buf_put(fs, b);
		return r ? r : -EAGAIN;
	}
	const struct dx_root_info *info = (const void *)(b->data + 24);
	*version = info->hash_version;
	int levels = info->indirect_levels;
	if (levels >= max_levels(fs) || info->hash_version > EXT4_HASH_TEA_UNSIGNED) {
		buf_put(fs, b);
		return -EAGAIN;
	}
	uint32_t off = DX_ROOT_OFF;
	for (int lvl = 0;; lvl++) {
		struct dx_countlimit *cl = (void *)(b->data + off);
		struct dx_entry *e = (void *)(b->data + off);
		uint32_t count = cl->count;
		int lo = 1, hi = (int)count - 1;
		while (lo <= hi) {
			int m = lo + (hi - lo) / 2;
			if (e[m].hash > hash)
				hi = m - 1;
			else
				lo = m + 1;
		}
		fr[lvl].buf = b;
		fr[lvl].off = off;
		fr[lvl].entries = e;
		fr[lvl].at = (uint32_t)(lo - 1);
		if (lvl == levels)
			return levels + 1;
		uint64_t child = e[lo - 1].block & 0x0fffffff;
		r = dir_read_block(fs, dir, child, &b);
		if (r) {
			frames_release(fs, fr, lvl + 1);
			return r;
		}
		r = dir_check_block(fs, dir, b->data, child, &dx);
		if (r || !dx) {
			buf_put(fs, b);
			frames_release(fs, fr, lvl + 1);
			return r ? r : -EAGAIN;
		}
		off = DX_NODE_OFF;
	}
}


/* ---- inline directories (inline_data feature, read only) ---------------- */

#define INLINE_DOTDOT 4u

struct inline_area {
	uint8_t buf[L2M_MAX_INODE_SIZE + 64];
	uint32_t ilen; /* bytes in i_block part (60) */
	uint32_t xlen; /* bytes from system.data */
};

static int inline_load(l2m_fs *fs, struct l2m_inode *dir, struct inline_area *a)
{
	memcpy(a->buf, dir->i.i_block, sizeof(dir->i.i_block));
	a->ilen = sizeof(dir->i.i_block);
	int x = xattr_get(fs, dir, EXT4_XATTR_INDEX_SYSTEM, "data", a->buf + a->ilen, sizeof(a->buf) - a->ilen);
	a->xlen = x > 0 ? (uint32_t)x : 0;
	return 0;
}

/* walk entries of one inline region; base = cookie offset of region */
static int inline_walk(l2m_fs *fs, uint8_t *p, uint32_t len, uint64_t base, uint64_t start, l2m_dirent_fn fn, void *ctx, uint64_t *cookie,
		       const char *name, size_t nlen, uint32_t *found, uint8_t *ftype)
{
	for (uint32_t off = 0; off + 8 <= len;) {
		struct ext4_dir_entry_2 *d = (void *)(p + off);
		if (d->rec_len < 8 || off + d->rec_len > len || (uint32_t)d->name_len + 8 > d->rec_len)
			return l2m_corrupt(fs, "bad inline directory entry", 0);
		uint64_t pos = base + off, next = base + off + d->rec_len;
		if (d->inode) {
			if (name) {
				if (d->name_len == nlen && memcmp(d->name, name, nlen) == 0) {
					*found = d->inode;
					if (ftype)
						*ftype = d->file_type;
					return 1;
				}
			} else if (pos >= start) {
				int r = fn(ctx, d->name, d->name_len, d->inode, fs->has_filetype ? d->file_type : 0, next);
				if (r) {
					if (r > 0)
						*cookie = pos;
					return r < 0 ? r : 2;
				}
			}
		}
		off += d->rec_len;
	}
	return 0;
}

static int inline_iterate(l2m_fs *fs, struct l2m_inode *dir, uint64_t *cookie, l2m_dirent_fn fn, void *ctx)
{
	struct inline_area a;
	inline_load(fs, dir, &a);
	uint64_t c = *cookie;
	int r;
	if (c == 0) {
		r = fn(ctx, ".", 1, dir->ino, EXT4_FT_DIR, 1);
		if (r)
			return r < 0 ? r : 0;
		c = 1;
	}
	if (c == 1) {
		r = fn(ctx, "..", 2, le32_at(a.buf), EXT4_FT_DIR, INLINE_DOTDOT);
		if (r) {
			*cookie = 1;
			return r < 0 ? r : 0;
		}
		c = INLINE_DOTDOT;
	}
	r = inline_walk(fs, a.buf + INLINE_DOTDOT, a.ilen - INLINE_DOTDOT, INLINE_DOTDOT, c, fn, ctx, cookie, NULL, 0, NULL, NULL);
	if (r == 2)
		return 0;
	if (r < 0)
		return r;
	if (a.xlen) {
		r = inline_walk(fs, a.buf + a.ilen, a.xlen, a.ilen, c, fn, ctx, cookie, NULL, 0, NULL, NULL);
		if (r == 2)
			return 0;
		if (r < 0)
			return r;
	}
	*cookie = (uint64_t)a.ilen + a.xlen;
	return 0;
}

static int inline_find(l2m_fs *fs, struct l2m_inode *dir, const char *name, size_t len, uint32_t *ino, uint8_t *ft)
{
	struct inline_area a;
	inline_load(fs, dir, &a);
	if (len == 1 && name[0] == '.') {
		*ino = dir->ino;
		return 0;
	}
	if (len == 2 && name[0] == '.' && name[1] == '.') {
		*ino = le32_at(a.buf);
		return 0;
	}
	int r = inline_walk(fs, a.buf + INLINE_DOTDOT, a.ilen - INLINE_DOTDOT, 0, 0, NULL, NULL, NULL, name, len, ino, ft);
	if (r == 0 && a.xlen)
		r = inline_walk(fs, a.buf + a.ilen, a.xlen, 0, 0, NULL, NULL, NULL, name, len, ino, ft);
	return r == 1 ? 0 : (r < 0 ? r : -ENOENT);
}

/* ---- searching ------------------------------------------------------------ */

struct found {
	uint64_t lblk;
	uint32_t off, prev; /* prev = UINT32_MAX if first in block */
	uint32_t ino;
	uint8_t ftype;
};

static int search_block(l2m_fs *fs, uint8_t *b, const char *name, size_t len, struct found *f)
{
	uint32_t end = fs->has_csum && has_tail(fs, b) ? fs->bs - TAIL : fs->bs, prev = UINT32_MAX;
	for (uint32_t off = 0; off < end;) {
		struct ext4_dir_entry_2 *d = de_at(b, off);
		if (d->rec_len < 8)
			break;
		if (d->inode && d->name_len == len && memcmp(d->name, name, len) == 0) {
			f->off = off;
			f->prev = prev;
			f->ino = d->inode;
			f->ftype = fs->has_filetype ? d->file_type : 0;
			return 1;
		}
		prev = off;
		off += d->rec_len;
	}
	return 0;
}

static uint64_t dir_nblocks(l2m_fs *fs, struct l2m_inode *dir) { return inode_size(dir) / fs->bs; }

static int linear_find(l2m_fs *fs, struct l2m_inode *dir, const char *name, size_t len, struct found *f)
{
	uint64_t n = dir_nblocks(fs, dir);
	for (uint64_t lb = 0; lb < n; lb++) {
		struct l2m_buf *b;
		bool dx;
		int r = dir_read_block(fs, dir, lb, &b);
		if (r == -ENODATA)
			continue;
		if (r)
			return r;
		r = dir_check_block(fs, dir, b->data, lb, &dx);
		if (r) {
			buf_put(fs, b);
			return r;
		}
		if (dx && lb != 0) {
			buf_put(fs, b);
			continue;
		}
		if (search_block(fs, b->data, name, len, f)) {
			f->lblk = lb;
			buf_put(fs, b);
			return 0;
		}
		buf_put(fs, b);
	}
	return -ENOENT;
}

static bool is_dot(const char *name, size_t len) { return (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'); }

static int find_entry(l2m_fs *fs, struct l2m_inode *dir, const char *name, size_t len, struct found *f)
{
	if (len == 0 || len > EXT4_NAME_LEN)
		return len ? -ENAMETOOLONG : -ENOENT;
	if (!((dir->i.i_flags & EXT4_INDEX_FL) && fs->has_dir_index) || is_dot(name, len))
		return linear_find(fs, dir, name, len, f);
	struct dx_frame fr[3];
	int version;
	uint32_t hash;
	/* hash version is needed before probing: read it from the root */
	struct l2m_buf *b0;
	int r = dir_read_block(fs, dir, 0, &b0);
	if (r)
		return r;
	version = ((struct dx_root_info *)(b0->data + 24))->hash_version;
	buf_put(fs, b0);
	if (name_hash(fs, version, name, len, &hash, NULL))
		return linear_find(fs, dir, name, len, f);
	int n = dx_probe(fs, dir, hash, fr, &version);
	if (n == -EAGAIN) {
		l2m_log(fs, L2M_LOG_WARN, "directory %u: htree unusable, using linear search", dir->ino);
		return linear_find(fs, dir, name, len, f);
	}
	if (n < 0)
		return n;
	for (;;) {
		struct dx_frame *lf = &fr[n - 1];
		uint64_t lb = lf->entries[lf->at].block & 0x0fffffff;
		struct l2m_buf *b;
		bool dx;
		r = dir_read_block(fs, dir, lb, &b);
		if (!r) {
			r = dir_check_block(fs, dir, b->data, lb, &dx);
			if (!r && !dx && search_block(fs, b->data, name, len, f)) {
				f->lblk = lb;
				buf_put(fs, b);
				frames_release(fs, fr, n);
				return 0;
			}
			buf_put(fs, b);
		}
		if (r && r != -ENODATA)
			break;
		/* hash collision continues in the next leaf? */
		int lvl = n - 1;
		while (lvl >= 0 && fr[lvl].at + 1 >= ((struct dx_countlimit *)fr[lvl].entries)->count)
			lvl--;
		if (lvl < 0) {
			r = -ENOENT;
			break;
		}
		uint32_t nh = fr[lvl].entries[fr[lvl].at + 1].hash;
		if (!(nh & 1) || (nh & ~1u) != hash) {
			r = -ENOENT;
			break;
		}
		fr[lvl].at++;
		/* descend to the first leaf below the new position */
		for (int k = lvl + 1; k < n; k++) {
			uint64_t child = fr[k - 1].entries[fr[k - 1].at].block & 0x0fffffff;
			struct l2m_buf *nb;
			r = dir_read_block(fs, dir, child, &nb);
			if (r)
				break;
			buf_put(fs, fr[k].buf);
			fr[k].buf = nb;
			fr[k].off = DX_NODE_OFF;
			fr[k].entries = (void *)(nb->data + DX_NODE_OFF);
			fr[k].at = 0;
		}
		if (r)
			break;
	}
	frames_release(fs, fr, n);
	return r;
}

int dir_lookup(l2m_fs *fs, struct l2m_inode *dir, const char *name, size_t len, uint32_t *ino, uint8_t *ftype)
{
	struct found f;
	if (!inode_is_dir(dir))
		return -ENOTDIR;
	if (dir->i.i_flags & EXT4_INLINE_DATA_FL)
		return inline_find(fs, dir, name, len, ino, ftype);
	int r = find_entry(fs, dir, name, len, &f);
	if (r)
		return r;
	*ino = f.ino;
	if (ftype)
		*ftype = f.ftype;
	return 0;
}

/* ---- iteration ------------------------------------------------------------- */

int dir_iterate(l2m_fs *fs, struct l2m_inode *dir, uint64_t *cookie, l2m_dirent_fn fn, void *ctx)
{
	if (dir->i.i_flags & EXT4_INLINE_DATA_FL)
		return inline_iterate(fs, dir, cookie, fn, ctx);
	uint64_t n = dir_nblocks(fs, dir);
	uint64_t lb = *cookie / fs->bs;
	uint32_t start = (uint32_t)(*cookie % fs->bs);
	for (; lb < n; lb++, start = 0) {
		struct l2m_buf *b;
		bool dx;
		int r = dir_read_block(fs, dir, lb, &b);
		if (r == -ENODATA) {
			*cookie = (lb + 1) * fs->bs;
			continue;
		}
		if (r)
			return r;
		r = dir_check_block(fs, dir, b->data, lb, &dx);
		if (r) {
			buf_put(fs, b);
			return r;
		}
		if (dx && lb != 0) {
			buf_put(fs, b);
			*cookie = (lb + 1) * fs->bs;
			continue;
		}
		uint32_t end = (fs->has_csum && has_tail(fs, b->data)) ? fs->bs - TAIL : fs->bs;
		if (dx)
			end = 24; /* dx root: only "." and ".." are entries */
		for (uint32_t off = 0; off < end;) {
			struct ext4_dir_entry_2 *d = de_at(b->data, off);
			uint32_t rl = d->rec_len;
			if (dx && off == DOT_REC)
				rl = fs->bs - DOT_REC;
			uint32_t next = off + rl;
			if (off >= start && d->inode) {
				uint64_t nc = next >= end ? (lb + 1) * fs->bs : lb * fs->bs + next;
				r = fn(ctx, d->name, d->name_len, d->inode, fs->has_filetype ? d->file_type : 0, nc);
				if (r) {
					buf_put(fs, b);
					if (r > 0) {
						*cookie = lb * fs->bs + off;
						return 0;
					}
					return r;
				}
			}
			off = next;
		}
		buf_put(fs, b);
		*cookie = (lb + 1) * fs->bs;
	}
	*cookie = n * fs->bs;
	return 0;
}

/* ---- modification ------------------------------------------------------------- */

static int block_dirty(l2m_fs *fs, struct l2m_inode *dir, struct l2m_buf *b, bool dx_root)
{
	if (dx_root)
		dx_set_csum(fs, dir, b->data, DX_ROOT_OFF);
	else
		leaf_set_tail(fs, dir, b->data);
	return buf_dirty(fs, b);
}

/* Try to place an entry into a leaf block. 1 = done, 0 = no room. */
static int add_to_block(l2m_fs *fs, uint8_t *b, uint32_t end, const char *name, size_t len, uint32_t ino, uint8_t ft)
{
	uint32_t need = EXT4_DIR_REC_LEN(len);
	for (uint32_t off = 0; off < end;) {
		struct ext4_dir_entry_2 *d = de_at(b, off);
		uint32_t used = d->inode ? EXT4_DIR_REC_LEN(d->name_len) : 0;
		if (d->rec_len >= used + need) {
			struct ext4_dir_entry_2 *n = d;
			if (used) {
				n = de_at(b, off + used);
				n->rec_len = (uint16_t)(d->rec_len - used);
				d->rec_len = (uint16_t)used;
			}
			n->inode = ino;
			n->name_len = (uint8_t)len;
			n->file_type = fs->has_filetype ? ft : 0;
			memcpy(n->name, name, len);
			/* zero the padding so the block is deterministic */
			uint32_t pad = EXT4_DIR_REC_LEN(len) - 8 - (uint32_t)len;
			memset(n->name + len, 0, pad);
			return 1;
		}
		if (d->rec_len < 12)
			return -EFSCORRUPTED;
		off += d->rec_len;
	}
	return 0;
}

static int init_leaf(l2m_fs *fs, uint8_t *b)
{
	memset(b, 0, fs->bs);
	struct ext4_dir_entry_2 *d = de_at(b, 0);
	d->inode = 0;
	d->rec_len = (uint16_t)leaf_end(fs);
	return 0;
}

/* Append a block to the directory, returns its buffer (zeroed). */
static int dir_append_block(l2m_fs *fs, struct l2m_inode *dir, uint64_t *lblk, struct l2m_buf **out)
{
	uint64_t lb = dir_nblocks(fs, dir);
	if (!fs->has_largedir && lb >= (1u << 21))
		return -ENOSPC;
	struct l2m_map m;
	int r = bmap_alloc(fs, dir, lb, 1, false, 0, &m);
	if (r)
		return r;
	if (m.lblk != lb || !m.pblk)
		return -EIO;
	r = buf_get_new(fs, m.pblk, out);
	if (r)
		return r;
	inode_set_size(dir, (lb + 1) * fs->bs);
	*lblk = lb;
	return 0;
}

struct mapent {
	uint32_t hash, minor;
	uint32_t off, size;
};

static int cmp_map(const void *a, const void *b)
{
	const struct mapent *x = a, *y = b;
	if (x->hash != y->hash)
		return x->hash < y->hash ? -1 : 1;
	if (x->minor != y->minor)
		return x->minor < y->minor ? -1 : 1;
	return 0;
}

/* split a full leaf by hash; returns the hash (with continuation bit) of
 * the new block and the new block's logical number */
static int dx_split_leaf(l2m_fs *fs, struct l2m_inode *dir, int version, struct l2m_buf *old, uint64_t *new_lblk, uint32_t *hash2, struct l2m_buf **newbuf)
{
	uint32_t end = leaf_end(fs);
	uint8_t *copy = malloc(fs->bs);
	struct mapent *map = malloc(sizeof(*map) * (fs->bs / 12 + 1));
	int r = 0;
	if (!copy || !map) {
		free(copy);
		free(map);
		return -ENOMEM;
	}
	memcpy(copy, old->data, fs->bs);
	uint32_t count = 0;
	for (uint32_t off = 0; off < end;) {
		struct ext4_dir_entry_2 *d = de_at(copy, off);
		if (d->inode) {
			map[count].off = off;
			map[count].size = EXT4_DIR_REC_LEN(d->name_len);
			name_hash(fs, version, d->name, d->name_len, &map[count].hash, &map[count].minor);
			count++;
		}
		off += d->rec_len;
	}
	if (count < 2) {
		free(copy);
		free(map);
		return -ENOSPC; /* cannot split a block with a single huge entry */
	}
	qsort(map, count, sizeof(*map), cmp_map);
	/* move entries from the end until about half the space */
	uint32_t size = 0, move = 0;
	for (int i = (int)count - 1; i >= 0; i--) {
		if (size + map[i].size / 2 > fs->bs / 2)
			break;
		size += map[i].size;
		move++;
	}
	if (move == 0)
		move = 1;
	if (move >= count)
		move = count - 1;
	uint32_t split = count - move;
	uint32_t h2 = map[split].hash;
	bool cont = h2 == map[split - 1].hash;

	struct l2m_buf *nb;
	uint64_t lb;
	r = dir_append_block(fs, dir, &lb, &nb);
	if (r) {
		free(copy);
		free(map);
		return r;
	}
	/* rebuild both blocks compactly */
	uint8_t *dst[2] = {old->data, nb->data};
	memset(old->data, 0, fs->bs);
	uint32_t pos[2] = {0, 0};
	struct ext4_dir_entry_2 *last[2] = {NULL, NULL};
	for (uint32_t i = 0; i < count; i++) {
		int w = i >= split;
		struct ext4_dir_entry_2 *s = de_at(copy, map[i].off);
		struct ext4_dir_entry_2 *d = de_at(dst[w], pos[w]);
		memcpy(d, s, map[i].size);
		d->rec_len = (uint16_t)map[i].size;
		last[w] = d;
		pos[w] += map[i].size;
	}
	for (int w = 0; w < 2; w++)
		last[w]->rec_len = (uint16_t)(last[w]->rec_len + (end - pos[w]));
	free(copy);
	free(map);
	*new_lblk = lb;
	*hash2 = h2 | (cont ? 1u : 0u);
	*newbuf = nb;
	return 0;
}

static int dx_node_dirty(l2m_fs *fs, struct l2m_inode *dir, struct dx_frame *f)
{
	dx_set_csum(fs, dir, f->buf->data, f->off);
	return buf_dirty(fs, f->buf);
}

static void dx_insert_entry(struct dx_frame *f, uint32_t hash, uint32_t block)
{
	struct dx_countlimit *cl = (void *)f->entries;
	struct dx_entry *e = f->entries;
	uint32_t at = f->at + 1;
	memmove(&e[at + 1], &e[at], (cl->count - at) * sizeof(*e));
	e[at].hash = hash;
	e[at].block = block;
	cl->count++;
}

/* move all entries of the root into a new index node (adds a level) */
static int dx_add_level(l2m_fs *fs, struct l2m_inode *dir, struct dx_frame *root)
{
	struct l2m_buf *nb;
	uint64_t lb;
	int r = dir_append_block(fs, dir, &lb, &nb);
	if (r)
		return r;
	struct dx_countlimit *rcl = (void *)root->entries;
	memset(nb->data, 0, fs->bs);
	struct ext4_dir_entry_2 *fake = de_at(nb->data, 0);
	fake->inode = 0;
	fake->rec_len = (uint16_t)fs->bs;
	struct dx_entry *ne = (void *)(nb->data + DX_NODE_OFF);
	memcpy(ne, root->entries, rcl->count * sizeof(*ne));
	struct dx_countlimit *ncl = (void *)ne;
	ncl->limit = (uint16_t)dx_limit(fs, DX_NODE_OFF);
	ncl->count = rcl->count;
	dx_set_csum(fs, dir, nb->data, DX_NODE_OFF);
	r = buf_dirty(fs, nb);
	buf_put(fs, nb);
	if (r)
		return r;
	rcl->count = 1;
	root->entries[0].block = (uint32_t)lb;
	struct dx_root_info *info = (void *)(root->buf->data + 24);
	info->indirect_levels++;
	return dx_node_dirty(fs, dir, root);
}

/* split a full (non-root) index node; parent has room */
static int dx_split_node(l2m_fs *fs, struct l2m_inode *dir, struct dx_frame *node, struct dx_frame *parent)
{
	struct dx_countlimit *cl = (void *)node->entries;
	uint32_t count = cl->count, keep = count / 2, moved = count - keep;
	struct l2m_buf *nb;
	uint64_t lb;
	int r = dir_append_block(fs, dir, &lb, &nb);
	if (r)
		return r;
	memset(nb->data, 0, fs->bs);
	struct ext4_dir_entry_2 *fake = de_at(nb->data, 0);
	fake->rec_len = (uint16_t)fs->bs;
	struct dx_entry *ne = (void *)(nb->data + DX_NODE_OFF);
	memcpy(ne, &node->entries[keep], moved * sizeof(*ne));
	uint32_t hash = ne[0].hash; /* entry 0 of the new node: its hash goes up */
	struct dx_countlimit *ncl = (void *)ne;
	ncl->limit = (uint16_t)dx_limit(fs, DX_NODE_OFF);
	ncl->count = (uint16_t)moved;
	dx_set_csum(fs, dir, nb->data, DX_NODE_OFF);
	r = buf_dirty(fs, nb);
	buf_put(fs, nb);
	if (r)
		return r;
	cl->count = (uint16_t)keep;
	r = dx_node_dirty(fs, dir, node);
	if (r)
		return r;
	dx_insert_entry(parent, hash, (uint32_t)lb);
	return dx_node_dirty(fs, dir, parent);
}

static int dx_add(l2m_fs *fs, struct l2m_inode *dir, const char *name, size_t len, uint32_t ino, uint8_t ft)
{
	for (int iter = 0; iter < 8; iter++) {
		struct dx_frame fr[3];
		int version;
		uint32_t hash;
		struct l2m_buf *b0;
		int r = dir_read_block(fs, dir, 0, &b0);
		if (r)
			return r;
		version = ((struct dx_root_info *)(b0->data + 24))->hash_version;
		buf_put(fs, b0);
		r = name_hash(fs, version, name, len, &hash, NULL);
		if (r)
			return r;
		int n = dx_probe(fs, dir, hash, fr, &version);
		if (n == -EAGAIN)
			return l2m_corrupt(fs, "cannot add to directory with damaged index", dir->ino);
		if (n < 0)
			return n;
		struct dx_frame *lf = &fr[n - 1];
		uint64_t lb = lf->entries[lf->at].block & 0x0fffffff;
		struct l2m_buf *leaf;
		bool dx;
		r = dir_read_block(fs, dir, lb, &leaf);
		if (!r)
			r = dir_check_block(fs, dir, leaf->data, lb, &dx);
		if (r) {
			frames_release(fs, fr, n);
			return r;
		}
		r = add_to_block(fs, leaf->data, leaf_end(fs), name, len, ino, ft);
		if (r == 1) {
			r = block_dirty(fs, dir, leaf, false);
			buf_put(fs, leaf);
			frames_release(fs, fr, n);
			return r;
		}
		if (r < 0) {
			buf_put(fs, leaf);
			frames_release(fs, fr, n);
			return r;
		}
		struct dx_countlimit *cl = (void *)lf->entries;
		if (cl->count >= cl->limit) {
			/* index node full */
			buf_put(fs, leaf);
			if (n == 1) {
				r = (n < max_levels(fs)) ? dx_add_level(fs, dir, &fr[0]) : -ENOSPC;
			} else {
				struct dx_frame *parent = &fr[n - 2];
				struct dx_countlimit *pcl = (void *)parent->entries;
				if (pcl->count < pcl->limit)
					r = dx_split_node(fs, dir, lf, parent);
				else if (n - 2 == 0 && n < max_levels(fs))
					r = dx_add_level(fs, dir, &fr[0]); /* parent is the full root */
				else
					r = -ENOSPC;
			}
			frames_release(fs, fr, n);
			if (r)
				return r;
			continue; /* probe again */
		}
		/* split the leaf */
		uint64_t nlb;
		uint32_t h2;
		struct l2m_buf *nb;
		r = dx_split_leaf(fs, dir, version, leaf, &nlb, &h2, &nb);
		if (r) {
			buf_put(fs, leaf);
			frames_release(fs, fr, n);
			return r;
		}
		dx_insert_entry(lf, h2, (uint32_t)nlb);
		r = dx_node_dirty(fs, dir, lf);
		struct l2m_buf *target = (hash >= (h2 & ~1u)) ? nb : leaf;
		if (!r) {
			int a = add_to_block(fs, target->data, leaf_end(fs), name, len, ino, ft);
			if (a == 0) {
				/* very rare: try the other half */
				target = target == nb ? leaf : nb;
				a = add_to_block(fs, target->data, leaf_end(fs), name, len, ino, ft);
			}
			r = a == 1 ? 0 : (a < 0 ? a : -ENOSPC);
		}
		if (!r)
			r = block_dirty(fs, dir, leaf, false);
		if (!r)
			r = block_dirty(fs, dir, nb, false);
		buf_put(fs, leaf);
		buf_put(fs, nb);
		frames_release(fs, fr, n);
		return r;
	}
	return l2m_corrupt(fs, "htree insert did not converge", dir->ino);
}

/* Convert a full one-block directory into an indexed directory. */
static int make_indexed(l2m_fs *fs, struct l2m_inode *dir)
{
	struct l2m_buf *b0, *b1;
	int r = dir_read_block(fs, dir, 0, &b0);
	if (r)
		return r;
	struct ext4_dir_entry_2 *dot = de_at(b0->data, 0);
	struct ext4_dir_entry_2 *dotdot = de_at(b0->data, dot->rec_len);
	if (dot->rec_len != DOT_REC || dotdot->name_len != 2 || memcmp(dotdot->name, "..", 2) != 0) {
		buf_put(fs, b0);
		return -EAGAIN; /* unusual layout: stay linear */
	}
	uint64_t lb;
	r = dir_append_block(fs, dir, &lb, &b1);
	if (r) {
		buf_put(fs, b0);
		return r;
	}
	/* copy all entries behind ".." into block 1 */
	uint32_t end = leaf_end(fs), src = DOT_REC + dotdot->rec_len;
	init_leaf(fs, b1->data);
	uint32_t pos = 0;
	struct ext4_dir_entry_2 *lastd = NULL;
	for (uint32_t off = src; off < end;) {
		struct ext4_dir_entry_2 *d = de_at(b0->data, off);
		if (d->rec_len < 12)
			break;
		if (d->inode) {
			uint32_t sz = EXT4_DIR_REC_LEN(d->name_len);
			memcpy(b1->data + pos, d, sz);
			lastd = de_at(b1->data, pos);
			lastd->rec_len = (uint16_t)sz;
			pos += sz;
		}
		off += d->rec_len;
	}
	if (lastd)
		lastd->rec_len = (uint16_t)(lastd->rec_len + (end - pos));
	else
		init_leaf(fs, b1->data);
	/* block 0 becomes the dx root */
	uint32_t dotdot_ino = dotdot->inode;
	uint8_t dotdot_ft = dotdot->file_type;
	uint32_t dot_ino = dot->inode;
	memset(b0->data, 0, fs->bs);
	dot = de_at(b0->data, 0);
	dot->inode = dot_ino;
	dot->rec_len = DOT_REC;
	dot->name_len = 1;
	dot->file_type = fs->has_filetype ? EXT4_FT_DIR : 0;
	dot->name[0] = '.';
	dotdot = de_at(b0->data, DOT_REC);
	dotdot->inode = dotdot_ino;
	dotdot->rec_len = (uint16_t)(fs->bs - DOT_REC);
	dotdot->name_len = 2;
	dotdot->file_type = dotdot_ft;
	memcpy(dotdot->name, "..", 2);
	struct dx_root_info *info = (void *)(b0->data + 24);
	info->reserved_zero = 0;
	info->hash_version = (uint8_t)(fs->def_hash <= EXT4_HASH_TEA ? fs->def_hash : EXT4_HASH_HALF_MD4);
	info->info_length = 8;
	info->indirect_levels = 0;
	info->unused_flags = 0;
	struct dx_countlimit *cl = (void *)(b0->data + DX_ROOT_OFF);
	cl->limit = (uint16_t)dx_limit(fs, DX_ROOT_OFF);
	cl->count = 1;
	((struct dx_entry *)cl)[0].block = (uint32_t)lb;
	dir->i.i_flags |= EXT4_INDEX_FL;
	r = block_dirty(fs, dir, b0, true);
	if (!r)
		r = block_dirty(fs, dir, b1, false);
	buf_put(fs, b0);
	buf_put(fs, b1);
	return r;
}

int dir_add(l2m_fs *fs, struct l2m_inode *dir, const char *name, size_t len, uint32_t ino, uint8_t ft)
{
	if (len == 0)
		return -EINVAL;
	if (len > EXT4_NAME_LEN)
		return -ENAMETOOLONG;
	if (memchr(name, '/', len) || memchr(name, 0, len))
		return -EINVAL;
	if ((dir->i.i_flags & EXT4_INDEX_FL) && fs->has_dir_index)
		return dx_add(fs, dir, name, len, ino, ft);
	if (dir->i.i_flags & EXT4_INDEX_FL)
		dir->i.i_flags &= ~(uint32_t)EXT4_INDEX_FL; /* index without dir_index feature: ignore */
	uint64_t n = dir_nblocks(fs, dir);
	for (uint64_t lb = 0; lb < n; lb++) {
		struct l2m_buf *b;
		bool dx;
		int r = dir_read_block(fs, dir, lb, &b);
		if (r == -ENODATA)
			continue;
		if (r)
			return r;
		r = dir_check_block(fs, dir, b->data, lb, &dx);
		if (r) {
			buf_put(fs, b);
			return r;
		}
		r = add_to_block(fs, b->data, leaf_end(fs), name, len, ino, ft);
		if (r == 1) {
			r = block_dirty(fs, dir, b, false);
			buf_put(fs, b);
			return r;
		}
		buf_put(fs, b);
		if (r < 0)
			return r;
	}
	if (n == 1 && fs->has_dir_index) {
		int r = make_indexed(fs, dir);
		if (r == 0)
			return dx_add(fs, dir, name, len, ino, ft);
		if (r != -EAGAIN)
			return r;
	}
	struct l2m_buf *b;
	uint64_t lb;
	int r = dir_append_block(fs, dir, &lb, &b);
	if (r)
		return r;
	init_leaf(fs, b->data);
	add_to_block(fs, b->data, leaf_end(fs), name, len, ino, ft);
	r = block_dirty(fs, dir, b, false);
	buf_put(fs, b);
	return r;
}

/* structural test only: the checksum is about to be recomputed */
static bool looks_like_dx_root(l2m_fs *fs, struct l2m_inode *dir, const uint8_t *b)
{
	if (!((dir->i.i_flags & EXT4_INDEX_FL) && fs->has_dir_index))
		return false;
	const struct ext4_dir_entry_2 *dot = (const void *)b;
	const struct dx_root_info *info = (const void *)(b + 24);
	const struct dx_countlimit *cl = (const void *)(b + DX_ROOT_OFF);
	return dot->rec_len == DOT_REC && info->reserved_zero == 0 && info->info_length == 8 && cl->limit == dx_limit(fs, DX_ROOT_OFF) &&
	       cl->count <= cl->limit;
}

static int write_back_block(l2m_fs *fs, struct l2m_inode *dir, uint64_t lb, struct l2m_buf *b)
{
	if (lb == 0 && looks_like_dx_root(fs, dir, b->data))
		return block_dirty(fs, dir, b, true);
	return block_dirty(fs, dir, b, false);
}

int dir_remove(l2m_fs *fs, struct l2m_inode *dir, const char *name, size_t len, uint32_t *ino)
{
	struct found f;
	if (is_dot(name, len))
		return -EINVAL;
	int r = find_entry(fs, dir, name, len, &f);
	if (r)
		return r;
	struct l2m_buf *b;
	r = dir_read_block(fs, dir, f.lblk, &b);
	if (r)
		return r;
	struct ext4_dir_entry_2 *d = de_at(b->data, f.off);
	if (ino)
		*ino = d->inode;
	if (f.prev != UINT32_MAX) {
		struct ext4_dir_entry_2 *p = de_at(b->data, f.prev);
		p->rec_len = (uint16_t)(p->rec_len + d->rec_len);
	} else {
		d->inode = 0;
	}
	r = write_back_block(fs, dir, f.lblk, b);
	buf_put(fs, b);
	return r;
}

int dir_set_entry(l2m_fs *fs, struct l2m_inode *dir, const char *name, size_t len, uint32_t ino, uint8_t ft)
{
	struct found f;
	int r = find_entry(fs, dir, name, len, &f);
	if (r)
		return r;
	struct l2m_buf *b;
	r = dir_read_block(fs, dir, f.lblk, &b);
	if (r)
		return r;
	struct ext4_dir_entry_2 *d = de_at(b->data, f.off);
	d->inode = ino;
	if (fs->has_filetype)
		d->file_type = ft;
	r = write_back_block(fs, dir, f.lblk, b);
	buf_put(fs, b);
	return r;
}

int dir_is_empty(l2m_fs *fs, struct l2m_inode *dir)
{
	uint64_t n = dir_nblocks(fs, dir);
	for (uint64_t lb = 0; lb < n; lb++) {
		struct l2m_buf *b;
		bool dx;
		int r = dir_read_block(fs, dir, lb, &b);
		if (r == -ENODATA)
			continue;
		if (r)
			return r;
		r = dir_check_block(fs, dir, b->data, lb, &dx);
		if (r) {
			buf_put(fs, b);
			return r;
		}
		if (dx) {
			buf_put(fs, b);
			continue;
		}
		uint32_t end = (fs->has_csum && has_tail(fs, b->data)) ? fs->bs - TAIL : fs->bs;
		for (uint32_t off = 0; off < end;) {
			struct ext4_dir_entry_2 *d = de_at(b->data, off);
			if (d->inode && !is_dot(d->name, d->name_len)) {
				buf_put(fs, b);
				return 0;
			}
			off += d->rec_len;
		}
		buf_put(fs, b);
	}
	return 1;
}

int dir_init(l2m_fs *fs, struct l2m_inode *dir, uint32_t parent)
{
	struct l2m_buf *b;
	uint64_t lb;
	int r = dir_append_block(fs, dir, &lb, &b);
	if (r)
		return r;
	memset(b->data, 0, fs->bs);
	struct ext4_dir_entry_2 *dot = de_at(b->data, 0);
	dot->inode = dir->ino;
	dot->rec_len = DOT_REC;
	dot->name_len = 1;
	dot->file_type = fs->has_filetype ? EXT4_FT_DIR : 0;
	dot->name[0] = '.';
	struct ext4_dir_entry_2 *dd = de_at(b->data, DOT_REC);
	dd->inode = parent;
	dd->rec_len = (uint16_t)(leaf_end(fs) - DOT_REC);
	dd->name_len = 2;
	dd->file_type = fs->has_filetype ? EXT4_FT_DIR : 0;
	memcpy(dd->name, "..", 2);
	r = block_dirty(fs, dir, b, false);
	buf_put(fs, b);
	return r;
}
