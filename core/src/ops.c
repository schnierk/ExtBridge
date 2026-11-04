/*
 * ExtBridge - public file system operations (namespace, data, xattr).
 *
 * Every public function takes fs->lock, runs one logical operation inside
 * the running transaction and ends with txn_end_op(), which commits when
 * the transaction is old or large enough.
 *
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

#include <time.h>

#define LOCK(fs) pthread_mutex_lock(&(fs)->lock)
#define UNLOCK(fs) pthread_mutex_unlock(&(fs)->lock)
#define EXT4_LINK_MAX 65000u
#define MAX_FILE_BLOCKS 0xFFFFFFFFull /* 2^32 logical blocks */

static int end_op(l2m_fs *fs, int r)
{
	int c = txn_end_op(fs);
	return r ? r : c;
}

static void koio_cut(l2m_fs *fs, uint32_t ino, uint64_t lblk, uint64_t end, bool only_op, uint64_t op);

/* ---- attributes -------------------------------------------------------------- */

static void fill_stat(l2m_fs *fs, struct l2m_inode *in, l2m_stat *st)
{
	memset(st, 0, sizeof(*st));
	st->ino = in->ino;
	st->mode = in->i.i_mode;
	st->nlink = in->i.i_links_count;
	if (inode_is_dir(in) && st->nlink == 1 && fs->has_dir_nlink)
		st->nlink = 1; /* "many" subdirectories */
	st->uid = inode_uid(in);
	st->gid = inode_gid(in);
	st->size = inode_size(in);
	st->alloc = inode_blocks512(fs, in) * 512;
	st->flags = in->i.i_flags;
	st->generation = in->i.i_generation;
	uint32_t t = in->i.i_mode & L2M_S_IFMT;
	if (t == L2M_S_IFCHR || t == L2M_S_IFBLK) {
		if (in->i.i_block[0]) {
			uint32_t d = in->i.i_block[0];
			st->rdev = ((d >> 8) & 0xff) << 20 | (d & 0xff); /* old encoding -> major<<20|minor */
		} else {
			uint32_t d = in->i.i_block[1];
			uint32_t major = (d & 0xfff00) >> 8, minor = (d & 0xff) | ((d >> 12) & 0xfff00);
			st->rdev = major << 20 | minor;
		}
	}
	inode_get_time(in, T_ATIME, &st->atime);
	inode_get_time(in, T_MTIME, &st->mtime);
	inode_get_time(in, T_CTIME, &st->ctime);
	inode_get_time(in, T_CRTIME, &st->crtime);
}

static int read_live_inode(l2m_fs *fs, uint32_t ino, struct l2m_inode *in)
{
	int r = inode_read(fs, ino, in);
	if (r)
		return r;
	if (in->i.i_links_count == 0 && in->i.i_mode == 0)
		return -ENOENT;
	return 0;
}

int l2m_getattr(l2m_fs *fs, uint32_t ino, l2m_stat *st)
{
	struct l2m_inode in;
	LOCK(fs);
	int r = read_live_inode(fs, ino, &in);
	if (!r)
		fill_stat(fs, &in, st);
	UNLOCK(fs);
	return r;
}

static int truncate_locked(l2m_fs *fs, struct l2m_inode *in, uint64_t size);

int l2m_setattr(l2m_fs *fs, uint32_t ino, unsigned mask, const l2m_stat *st)
{
	struct l2m_inode in;
	LOCK(fs);
	int r = fs_check_writable(fs);
	if (!r)
		r = read_live_inode(fs, ino, &in);
	if (r)
		goto out;
	if ((in.i.i_flags & (EXT4_IMMUTABLE_FL | EXT4_APPEND_FL)) && (mask & ~(unsigned)L2M_SET_ATIME) && !(mask & L2M_SET_FLAGS)) {
		r = -EPERM;
		goto out;
	}
	if (mask & L2M_SET_SIZE) {
		if (!inode_is_reg(&in)) {
			r = inode_is_dir(&in) ? -EISDIR : -EINVAL;
			goto out;
		}
		r = truncate_locked(fs, &in, st->size);
		if (r)
			goto out;
	}
	if (mask & L2M_SET_MODE)
		in.i.i_mode = (uint16_t)((in.i.i_mode & L2M_S_IFMT) | (st->mode & 07777));
	if (mask & L2M_SET_UID) {
		in.i.i_uid = (uint16_t)st->uid;
		in.i.i_uid_high = (uint16_t)(st->uid >> 16);
	}
	if (mask & L2M_SET_GID) {
		in.i.i_gid = (uint16_t)st->gid;
		in.i.i_gid_high = (uint16_t)(st->gid >> 16);
	}
	if (mask & L2M_SET_ATIME)
		inode_set_time(fs, &in, T_ATIME, &st->atime);
	if (mask & L2M_SET_MTIME)
		inode_set_time(fs, &in, T_MTIME, &st->mtime);
	if (mask & L2M_SET_CRTIME)
		inode_set_time(fs, &in, T_CRTIME, &st->crtime);
	if (mask & L2M_SET_FLAGS) {
		/* only flags a user may change with chattr */
		uint32_t user = EXT4_SYNC_FL | EXT4_IMMUTABLE_FL | EXT4_APPEND_FL | EXT4_NODUMP_FL | EXT4_NOATIME_FL;
		in.i.i_flags = (in.i.i_flags & ~user) | (st->flags & user);
	}
	if (mask & L2M_SET_CTIME)
		inode_set_time(fs, &in, T_CTIME, &st->ctime);
	else
		inode_touch(fs, &in, T_CTIME);
	r = inode_write(fs, &in);
out:
	r = end_op(fs, r);
	UNLOCK(fs);
	return r;
}

/* ---- lookups ----------------------------------------------------------------- */

int l2m_lookup(l2m_fs *fs, uint32_t dir, const char *name, size_t len, uint32_t *ino)
{
	struct l2m_inode d;
	LOCK(fs);
	int r = read_live_inode(fs, dir, &d);
	if (!r)
		r = dir_lookup(fs, &d, name, len, ino, NULL);
	UNLOCK(fs);
	return r;
}

int l2m_lookup_path(l2m_fs *fs, const char *path, uint32_t *ino)
{
	uint32_t cur = L2M_ROOT_INO;
	const char *p = path;
	while (*p) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		const char *e = strchr(p, '/');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		uint32_t next;
		int r = l2m_lookup(fs, cur, p, len, &next);
		if (r)
			return r;
		cur = next;
		p += len;
	}
	*ino = cur;
	return 0;
}

int l2m_readdir(l2m_fs *fs, uint32_t dir, uint64_t *cookie, l2m_dirent_fn fn, void *ctx)
{
	struct l2m_inode d;
	LOCK(fs);
	int r = read_live_inode(fs, dir, &d);
	if (!r && !inode_is_dir(&d))
		r = -ENOTDIR;
	if (!r)
		r = dir_iterate(fs, &d, cookie, fn, ctx);
	UNLOCK(fs);
	return r;
}

uint64_t l2m_dir_version(l2m_fs *fs, uint32_t dir)
{
	(void)dir;
	LOCK(fs);
	uint64_t v = fs->dir_gen + 1;
	UNLOCK(fs);
	return v;
}

/* ---- creating ------------------------------------------------------------------ */

static uint32_t new_generation(void)
{
	static uint32_t seed;
	if (!seed) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		seed = (uint32_t)ts.tv_nsec ^ (uint32_t)ts.tv_sec ^ 0x9E3779B9u;
	}
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return seed;
}

static int check_dir_writable(l2m_fs *fs, struct l2m_inode *dir)
{
	if (!inode_is_dir(dir))
		return -ENOTDIR;
	if (dir->i.i_flags & (EXT4_IMMUTABLE_FL | EXT4_APPEND_FL))
		return -EPERM;
	if (dir->i.i_flags & (EXT4_ENCRYPT_FL | EXT4_CASEFOLD_FL))
		return -EOPNOTSUPP;
	if (dir->i.i_links_count == 0)
		return -ENOENT;
	(void)fs;
	return 0;
}

static void init_new_inode(l2m_fs *fs, struct l2m_inode *in, uint32_t ino, struct l2m_inode *parent, uint32_t mode, uint32_t uid, uint32_t gid)
{
	memset(in, 0, sizeof(*in));
	in->ino = ino;
	in->i.i_mode = (uint16_t)mode;
	in->i.i_uid = (uint16_t)uid;
	in->i.i_uid_high = (uint16_t)(uid >> 16);
	/* BSD group semantics like Linux with setgid directories */
	if (parent->i.i_mode & 02000) {
		gid = inode_gid(parent);
		if ((mode & L2M_S_IFMT) == L2M_S_IFDIR)
			in->i.i_mode |= 02000;
	}
	in->i.i_gid = (uint16_t)gid;
	in->i.i_gid_high = (uint16_t)(gid >> 16);
	in->i.i_links_count = 1;
	in->i.i_generation = new_generation();
	if (fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE)
		in->i.i_extra_isize = fs->want_extra_isize;
	uint32_t inherit = EXT4_SYNC_FL | EXT4_NODUMP_FL | EXT4_NOATIME_FL | EXT4_PROJINHERIT_FL;
	in->i.i_flags = parent->i.i_flags & inherit;
	if ((parent->i.i_flags & EXT4_PROJINHERIT_FL) && inode_has_extra(fs, parent, offsetof(struct ext4_inode, i_projid) + 4))
		in->i.i_projid = parent->i.i_projid;
	inode_init_seed(fs, in);
	inode_touch(fs, in, T_ATIME | T_MTIME | T_CTIME | T_CRTIME);
}

static int create_locked(l2m_fs *fs, uint32_t dirino, const char *name, size_t len, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t rdev, const char *symlink, uint32_t *out)
{
	struct l2m_inode dir, in;
	int r = fs_check_writable(fs);
	if (r)
		return r;
	if (len == 0 || (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
		return -EEXIST;
	if (len > EXT4_NAME_LEN)
		return -ENAMETOOLONG;
	r = read_live_inode(fs, dirino, &dir);
	if (r)
		return r;
	r = check_dir_writable(fs, &dir);
	if (r)
		return r;
	uint32_t existing;
	r = dir_lookup(fs, &dir, name, len, &existing, NULL);
	if (r == 0)
		return -EEXIST;
	if (r != -ENOENT)
		return r;
	bool is_dir = (mode & L2M_S_IFMT) == L2M_S_IFDIR;
	if (is_dir && dir.i.i_links_count >= EXT4_LINK_MAX && !fs->has_dir_nlink)
		return -EMLINK;
	size_t tlen = symlink ? strlen(symlink) : 0;
	if (symlink && (tlen == 0 || tlen >= fs->bs))
		return tlen ? -ENAMETOOLONG : -ENOENT;
	/* rough room check so the operation does not run out of journal */
	r = txn_need_room(fs, 64);
	if (r)
		return r;

	uint32_t ino;
	r = alloc_inode(fs, dirino, is_dir, &ino);
	if (r)
		return r;
	init_new_inode(fs, &in, ino, &dir, mode, uid, gid);
	uint32_t type = mode & L2M_S_IFMT;
	if (type == L2M_S_IFREG || type == L2M_S_IFDIR)
		ext_init_root(&in);
	if (type == L2M_S_IFCHR || type == L2M_S_IFBLK) {
		uint32_t major = rdev >> 20, minor = rdev & 0xfffff;
		if (major < 256 && minor < 256)
			in.i.i_block[0] = major << 8 | minor;
		else
			in.i.i_block[1] = (minor & 0xff) | (major << 8) | ((minor & ~0xffu) << 12);
	}
	if (symlink) {
		if (tlen < sizeof(in.i.i_block)) {
			memcpy(in.i.i_block, symlink, tlen);
		} else {
			ext_init_root(&in);
			struct l2m_map m;
			r = bmap_alloc(fs, &in, 0, 1, false, 0, &m);
			if (r)
				goto undo_inode;
			uint8_t *blk = calloc(1, fs->bs);
			if (!blk) {
				r = -ENOMEM;
				goto undo_inode;
			}
			memcpy(blk, symlink, tlen);
			r = dev_write_data(fs, m.pblk, 1, blk);
			free(blk);
			if (r)
				goto undo_inode;
		}
		inode_set_size(&in, tlen);
	}
	if (is_dir) {
		in.i.i_links_count = 2;
		r = dir_init(fs, &in, dirino);
		if (r)
			goto undo_inode;
	}
	r = inode_write(fs, &in);
	if (r)
		goto undo_inode;
	r = dir_add(fs, &dir, name, len, ino, mode_to_ftype(mode));
	if (r)
		goto undo_all;
	if (is_dir) {
		if (dir.i.i_links_count >= EXT4_LINK_MAX - 1 && fs->has_dir_nlink)
			dir.i.i_links_count = 1;
		else if (dir.i.i_links_count != 1)
			dir.i.i_links_count++;
	}
	inode_touch(fs, &dir, T_MTIME | T_CTIME);
	r = inode_write(fs, &dir);
	if (r)
		return r;
	fs->dir_gen++;
	*out = ino;
	return 0;

undo_all:
undo_inode:
	/* give back what we took; the transaction stays consistent */
	{
		int e = bmap_truncate(fs, &in, 0);
		(void)e;
		in.i.i_links_count = 0;
		l2m_time now;
		l2m_now(&now);
		in.i.i_dtime = (uint32_t)now.sec;
		inode_write(fs, &in);
		free_inode(fs, ino, is_dir);
	}
	return r;
}

int l2m_create(l2m_fs *fs, uint32_t dir, const char *name, size_t len, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t rdev, uint32_t *ino)
{
	uint32_t t = mode & L2M_S_IFMT;
	if (t == 0)
		mode |= L2M_S_IFREG, t = L2M_S_IFREG;
	if (t == L2M_S_IFLNK)
		return -EINVAL;
	LOCK(fs);
	int r = create_locked(fs, dir, name, len, mode, uid, gid, rdev, NULL, ino);
	if (r == -ENOSPC && txn_enospc_retry(fs) == 1)
		r = create_locked(fs, dir, name, len, mode, uid, gid, rdev, NULL, ino);
	r = end_op(fs, r);
	UNLOCK(fs);
	return r;
}

int l2m_symlink(l2m_fs *fs, uint32_t dir, const char *name, size_t len, const char *target, uint32_t uid, uint32_t gid, uint32_t *ino)
{
	LOCK(fs);
	int r = create_locked(fs, dir, name, len, L2M_S_IFLNK | 0777, uid, gid, 0, target, ino);
	r = end_op(fs, r);
	UNLOCK(fs);
	return r;
}

static int64_t read_locked(l2m_fs *fs, struct l2m_inode *in, uint64_t off, void *buf, size_t len);

int l2m_readlink(l2m_fs *fs, uint32_t ino, char *buf, size_t buflen)
{
	struct l2m_inode in;
	LOCK(fs);
	int r = read_live_inode(fs, ino, &in);
	if (!r && !inode_is_lnk(&in))
		r = -EINVAL;
	if (!r) {
		uint64_t sz = inode_size(&in);
		if (sz >= buflen || sz > fs->bs) {
			r = -ENAMETOOLONG;
		} else if (inode_is_fast_symlink(fs, &in)) {
			memcpy(buf, in.i.i_block, sz);
			buf[sz] = 0;
			r = (int)sz;
		} else {
			int64_t n = read_locked(fs, &in, 0, buf, sz);
			if (n < 0)
				r = (int)n;
			else {
				buf[n] = 0;
				r = (int)n;
			}
		}
	}
	UNLOCK(fs);
	return r;
}

int l2m_link(l2m_fs *fs, uint32_t ino, uint32_t dirino, const char *name, size_t len)
{
	struct l2m_inode in, dir;
	LOCK(fs);
	int r = fs_check_writable(fs);
	if (!r)
		r = read_live_inode(fs, ino, &in);
	if (!r && inode_is_dir(&in))
		r = -EPERM;
	if (!r && in.i.i_links_count >= EXT4_LINK_MAX)
		r = -EMLINK;
	if (!r && (in.i.i_flags & (EXT4_IMMUTABLE_FL | EXT4_APPEND_FL)))
		r = -EPERM;
	if (!r)
		r = read_live_inode(fs, dirino, &dir);
	if (!r)
		r = check_dir_writable(fs, &dir);
	if (!r) {
		uint32_t x;
		r = dir_lookup(fs, &dir, name, len, &x, NULL);
		r = r == 0 ? -EEXIST : (r == -ENOENT ? 0 : r);
	}
	if (!r)
		r = dir_add(fs, &dir, name, len, ino, mode_to_ftype(in.i.i_mode));
	if (!r) {
		if (in.i.i_links_count == 0) {
			/* linking an orphan (open unlinked file) back */
			r = orphan_del(fs, &in);
		}
		in.i.i_links_count++;
		inode_touch(fs, &in, T_CTIME);
		if (!r)
			r = inode_write(fs, &in);
		inode_touch(fs, &dir, T_MTIME | T_CTIME);
		if (!r)
			r = inode_write(fs, &dir);
		fs->dir_gen++;
	}
	r = end_op(fs, r);
	UNLOCK(fs);
	return r;
}

/* ---- orphans ------------------------------------------------------------------- */

int orphan_add(l2m_fs *fs, struct l2m_inode *in)
{
	/* already on the list? */
	for (uint32_t cur = fs->sb->s_last_orphan, guard = 0; cur && guard < 100000; guard++) {
		if (cur == in->ino)
			return 0;
		struct l2m_inode o;
		if (inode_read(fs, cur, &o))
			break;
		cur = o.i.i_dtime;
	}
	in->i.i_dtime = fs->sb->s_last_orphan;
	fs->sb->s_last_orphan = in->ino;
	int r = sb_dirty(fs);
	if (!r)
		r = inode_write(fs, in);
	return r;
}

int orphan_del(l2m_fs *fs, struct l2m_inode *in)
{
	uint32_t next = in->i.i_dtime;
	if (fs->sb->s_last_orphan == in->ino) {
		fs->sb->s_last_orphan = next;
		in->i.i_dtime = 0;
		return sb_dirty(fs);
	}
	uint32_t cur = fs->sb->s_last_orphan;
	for (int guard = 0; cur && guard < 100000; guard++) {
		struct l2m_inode o;
		int r = inode_read(fs, cur, &o);
		if (r)
			return r;
		if (o.i.i_dtime == in->ino) {
			o.i.i_dtime = next;
			in->i.i_dtime = 0;
			return inode_write(fs, &o);
		}
		cur = o.i.i_dtime;
	}
	in->i.i_dtime = 0; /* was not on the list */
	return 0;
}

/* Frees an unlinked inode. The inode stays on the orphan list while its
 * blocks are released (a large truncate may commit in between), and is
 * taken off the list in the same transaction that frees it - so a crash at
 * any point leaves an orphan that Linux, e2fsck or our next mount finishes. */
static int free_inode_fully(l2m_fs *fs, struct l2m_inode *in)
{
	bool is_dir = inode_is_dir(in);
	int r = 0;
	if (!inode_is_fast_symlink(fs, in) && (in->i.i_mode & L2M_S_IFMT) != L2M_S_IFCHR &&
	    (in->i.i_mode & L2M_S_IFMT) != L2M_S_IFBLK && (in->i.i_mode & L2M_S_IFMT) != L2M_S_IFIFO &&
	    (in->i.i_mode & L2M_S_IFMT) != L2M_S_IFSOCK && !(in->i.i_flags & EXT4_INLINE_DATA_FL))
		r = bmap_truncate(fs, in, 0);
	koio_cut(fs, in->ino, 0, ~0ull, false, 0);
	if (!r)
		r = xattr_free_all(fs, in);
	if (!r)
		r = orphan_del(fs, in);
	if (r)
		return r;
	inode_set_size(in, 0);
	l2m_time now;
	l2m_now(&now);
	in->i.i_dtime = (uint32_t)now.sec;
	in->i.i_links_count = 0;
	r = inode_write(fs, in);
	if (!r)
		r = free_inode(fs, in->ino, is_dir);
	return r;
}

int orphan_cleanup(l2m_fs *fs)
{
	int n = 0;
	while (fs->sb->s_last_orphan && n < 100000) {
		uint32_t cur = fs->sb->s_last_orphan;
		struct l2m_inode in;
		int r = inode_read(fs, cur, &in);
		if (r) {
			l2m_log(fs, L2M_LOG_WARN, "orphan list broken at inode %u; leaving it to e2fsck", cur);
			return 0;
		}
		if (in.i.i_links_count == 0) {
			r = free_inode_fully(fs, &in);
		} else {
			/* truncate interrupted by a crash: finish it */
			uint64_t keep = L2M_DIV_ROUND_UP(inode_size(&in), fs->bs);
			if (inode_is_reg(&in))
				r = bmap_truncate(fs, &in, keep);
			if (!r)
				r = orphan_del(fs, &in);
			if (!r)
				r = inode_write(fs, &in);
		}
		if (r)
			return r;
		if (fs->sb->s_last_orphan == cur) {
			l2m_log(fs, L2M_LOG_WARN, "orphan list does not advance at inode %u; leaving it to e2fsck", cur);
			return 0;
		}
		l2m_log(fs, L2M_LOG_INFO, "cleaned up orphan inode %u", cur);
		n++;
	}
	return 0;
}

int l2m_release(l2m_fs *fs, uint32_t ino)
{
	struct l2m_inode in;
	LOCK(fs);
	int r = 0;
	if (fs->rdonly)
		goto out;
	r = inode_read(fs, ino, &in);
	if (r)
		goto out;
	if (in.i.i_links_count != 0 || in.i.i_mode == 0)
		goto out;
	/* may be called twice (last close, then reclaim): only free once */
	int e;
	if (!inode_allocated(fs, ino, &e) || e)
		goto out;
	r = free_inode_fully(fs, &in);
out:
	r = end_op(fs, r);
	UNLOCK(fs);
	return r;
}

/* ---- removing ------------------------------------------------------------------ */

static int drop_link(l2m_fs *fs, struct l2m_inode *in, int *orphan)
{
	if (inode_is_dir(in))
		in->i.i_links_count = 0;
	else if (in->i.i_links_count > 0)
		in->i.i_links_count--;
	inode_touch(fs, in, T_CTIME);
	if (in->i.i_links_count == 0) {
		if (orphan)
			*orphan = 1;
		return orphan_add(fs, in);
	}
	return inode_write(fs, in);
}

static void parent_drop_subdir(struct l2m_inode *parent)
{
	if (parent->i.i_links_count > 2)
		parent->i.i_links_count--;
	/* links == 1 means "too many to count" (dir_nlink): keep it */
}

int l2m_unlink(l2m_fs *fs, uint32_t dirino, const char *name, size_t len, int is_dir, int *orphan)
{
	struct l2m_inode dir, in;
	if (orphan)
		*orphan = 0;
	LOCK(fs);
	int r = fs_check_writable(fs);
	if (!r)
		r = read_live_inode(fs, dirino, &dir);
	if (!r)
		r = check_dir_writable(fs, &dir);
	uint32_t ino = 0;
	if (!r && ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')))
		r = -EINVAL;
	if (!r)
		r = dir_lookup(fs, &dir, name, len, &ino, NULL);
	if (!r)
		r = read_live_inode(fs, ino, &in);
	if (!r) {
		if (is_dir && !inode_is_dir(&in))
			r = -ENOTDIR;
		else if (!is_dir && inode_is_dir(&in))
			r = -EISDIR;
		else if (in.i.i_flags & (EXT4_IMMUTABLE_FL | EXT4_APPEND_FL))
			r = -EPERM;
	}
	if (!r && is_dir) {
		int e = dir_is_empty(fs, &in);
		r = e < 0 ? e : (e ? 0 : -ENOTEMPTY);
	}
	if (!r)
		r = txn_need_room(fs, 32);
	if (!r)
		r = dir_remove(fs, &dir, name, len, NULL);
	if (!r) {
		if (is_dir)
			parent_drop_subdir(&dir);
		inode_touch(fs, &dir, T_MTIME | T_CTIME);
		r = inode_write(fs, &dir);
	}
	if (!r)
		r = drop_link(fs, &in, orphan);
	if (!r)
		fs->dir_gen++;
	r = end_op(fs, r);
	UNLOCK(fs);
	return r;
}

/* is 'ancestor' an ancestor of (or equal to) dir? */
static int is_ancestor(l2m_fs *fs, uint32_t ancestor, uint32_t dir)
{
	uint32_t cur = dir;
	for (int depth = 0; depth < 4096; depth++) {
		if (cur == ancestor)
			return 1;
		if (cur == L2M_ROOT_INO)
			return 0;
		struct l2m_inode d;
		int r = inode_read(fs, cur, &d);
		if (r)
			return r;
		uint32_t up;
		r = dir_lookup(fs, &d, "..", 2, &up, NULL);
		if (r)
			return r;
		if (up == cur)
			return 0;
		cur = up;
	}
	return -ELOOP;
}

int l2m_rename(l2m_fs *fs, uint32_t sdir, const char *sname, size_t slen, uint32_t ddir, const char *dname, size_t dlen, unsigned flags, uint32_t *replaced_ino, int *replaced_orphan)
{
	struct l2m_inode sd, ddbuf, *dd, src, dst;
	if (replaced_ino)
		*replaced_ino = 0;
	if (replaced_orphan)
		*replaced_orphan = 0;
	LOCK(fs);
	int r = fs_check_writable(fs);
	uint32_t sino = 0, dino = 0;
	bool have_dst = false;
	if (!r && (slen == 0 || dlen == 0))
		r = -ENOENT;
	if (!r && ((slen <= 2 && memcmp(sname, "..", slen) == 0) || (dlen <= 2 && memcmp(dname, "..", dlen) == 0)))
		r = -EINVAL;
	if (!r && dlen > EXT4_NAME_LEN)
		r = -ENAMETOOLONG;
	if (!r)
		r = read_live_inode(fs, sdir, &sd);
	if (!r)
		r = check_dir_writable(fs, &sd);
	dd = &sd;
	if (!r && ddir != sdir) {
		dd = &ddbuf;
		r = read_live_inode(fs, ddir, dd);
		if (!r)
			r = check_dir_writable(fs, dd);
	}
	if (!r)
		r = dir_lookup(fs, &sd, sname, slen, &sino, NULL);
	if (!r)
		r = read_live_inode(fs, sino, &src);
	if (!r && (src.i.i_flags & (EXT4_IMMUTABLE_FL | EXT4_APPEND_FL)))
		r = -EPERM;
	if (!r) {
		int e = dir_lookup(fs, dd, dname, dlen, &dino, NULL);
		if (e == 0)
			have_dst = true;
		else if (e != -ENOENT)
			r = e;
	}
	if (r)
		goto out;
	if ((flags & L2M_RENAME_EXCHANGE) && !have_dst) {
		r = -ENOENT;
		goto out;
	}
	if (have_dst && (flags & L2M_RENAME_NOREPLACE)) {
		r = -EEXIST;
		goto out;
	}
	if (have_dst && dino == sino) {
		r = 0; /* same file: nothing to do (POSIX) */
		goto out;
	}
	bool src_dir = inode_is_dir(&src);
	if (have_dst) {
		r = read_live_inode(fs, dino, &dst);
		if (r)
			goto out;
	}
	bool dst_dir = have_dst && inode_is_dir(&dst);
	/* moving a directory into its own subtree */
	if (src_dir && sdir != ddir) {
		int a = is_ancestor(fs, sino, ddir);
		if (a < 0) {
			r = a;
			goto out;
		}
		if (a) {
			r = -EINVAL;
			goto out;
		}
	}
	if (have_dst && (flags & L2M_RENAME_EXCHANGE)) {
		if (dst_dir && sdir != ddir) {
			int a = is_ancestor(fs, dino, sdir);
			if (a) {
				r = a < 0 ? a : -EINVAL;
				goto out;
			}
		}
		r = txn_need_room(fs, 32);
		if (!r)
			r = dir_set_entry(fs, &sd, sname, slen, dino, mode_to_ftype(dst.i.i_mode));
		if (!r)
			r = dir_set_entry(fs, dd, dname, dlen, sino, mode_to_ftype(src.i.i_mode));
		if (!r && sdir != ddir) {
			if (src_dir)
				r = dir_set_entry(fs, &src, "..", 2, ddir, EXT4_FT_DIR);
			if (!r && dst_dir)
				r = dir_set_entry(fs, &dst, "..", 2, sdir, EXT4_FT_DIR);
			if (!r && src_dir != dst_dir) {
				if (src_dir) {
					parent_drop_subdir(&sd);
					if (dd->i.i_links_count != 1)
						dd->i.i_links_count++;
				} else {
					parent_drop_subdir(dd);
					if (sd.i.i_links_count != 1)
						sd.i.i_links_count++;
				}
			}
		}
		inode_touch(fs, &src, T_CTIME);
		inode_touch(fs, &dst, T_CTIME);
		if (!r)
			r = inode_write(fs, &src);
		if (!r)
			r = inode_write(fs, &dst);
		goto dirs;
	}
	if (have_dst) {
		if (src_dir && !dst_dir) {
			r = -ENOTDIR;
			goto out;
		}
		if (!src_dir && dst_dir) {
			r = -EISDIR;
			goto out;
		}
		if (dst.i.i_flags & (EXT4_IMMUTABLE_FL | EXT4_APPEND_FL)) {
			r = -EPERM;
			goto out;
		}
		if (dst_dir) {
			int e = dir_is_empty(fs, &dst);
			if (e <= 0) {
				r = e < 0 ? e : -ENOTEMPTY;
				goto out;
			}
		}
	}
	r = txn_need_room(fs, 48);
	if (r)
		goto out;
	if (have_dst) {
		r = dir_set_entry(fs, dd, dname, dlen, sino, mode_to_ftype(src.i.i_mode));
		if (!r) {
			if (dst_dir)
				parent_drop_subdir(dd);
			r = drop_link(fs, &dst, replaced_orphan);
			if (replaced_ino)
				*replaced_ino = dino;
		}
	} else {
		r = dir_add(fs, dd, dname, dlen, sino, mode_to_ftype(src.i.i_mode));
	}
	if (!r)
		r = dir_remove(fs, &sd, sname, slen, NULL);
	if (!r && src_dir && sdir != ddir) {
		r = dir_set_entry(fs, &src, "..", 2, ddir, EXT4_FT_DIR);
		parent_drop_subdir(&sd);
		if (!have_dst && dd->i.i_links_count != 1) {
			if (dd->i.i_links_count >= EXT4_LINK_MAX - 1 && fs->has_dir_nlink)
				dd->i.i_links_count = 1;
			else
				dd->i.i_links_count++;
		}
	}
	inode_touch(fs, &src, T_CTIME);
	if (!r)
		r = inode_write(fs, &src); /* also saves ".." block changes of src (size unchanged) */
dirs:
	inode_touch(fs, &sd, T_MTIME | T_CTIME);
	if (!r)
		r = inode_write(fs, &sd);
	if (dd != &sd) {
		inode_touch(fs, dd, T_MTIME | T_CTIME);
		if (!r)
			r = inode_write(fs, dd);
	}
	if (!r)
		fs->dir_gen++;
out:
	r = end_op(fs, r);
	UNLOCK(fs);
	return r;
}

/* ---- file data ------------------------------------------------------------------ */

static int read_inline(l2m_fs *fs, struct l2m_inode *in, uint64_t off, void *buf, size_t len, int64_t *done)
{
	/* inline data: first 60 bytes in i_block, the rest in system.data */
	uint8_t tmp[L2M_MAX_INODE_SIZE + 64];
	size_t have = sizeof(in->i.i_block);
	memcpy(tmp, in->i.i_block, have);
	int x = xattr_get(fs, in, EXT4_XATTR_INDEX_SYSTEM, "data", tmp + have, sizeof(tmp) - have);
	if (x > 0)
		have += (size_t)x;
	uint64_t size = L2M_MIN(inode_size(in), (uint64_t)have);
	if (off >= size) {
		*done = 0;
		return 0;
	}
	size_t n = (size_t)L2M_MIN((uint64_t)len, size - off);
	memcpy(buf, tmp + off, n);
	*done = (int64_t)n;
	return 0;
}

static int64_t read_locked(l2m_fs *fs, struct l2m_inode *in, uint64_t off, void *buf, size_t len)
{
	uint64_t size = inode_size(in);
	if (inode_is_dir(in))
		return -EISDIR;
	if (in->i.i_flags & EXT4_ENCRYPT_FL)
		return -EACCES;
	if (off >= size || len == 0)
		return 0;
	if (len > size - off)
		len = (size_t)(size - off);
	if (in->i.i_flags & EXT4_INLINE_DATA_FL) {
		int64_t d;
		int r = read_inline(fs, in, off, buf, len, &d);
		return r ? r : d;
	}
	uint8_t *out = buf, *bounce = NULL;
	size_t done = 0;
	while (done < len) {
		uint64_t pos = off + done;
		uint64_t lblk = pos / fs->bs;
		uint32_t boff = (uint32_t)(pos % fs->bs);
		struct l2m_map m;
		int r = bmap(fs, in, lblk, &m);
		if (r) {
			free(bounce);
			return r;
		}
		uint64_t span = (uint64_t)m.len * fs->bs - boff;
		size_t n = (size_t)L2M_MIN(span, (uint64_t)(len - done));
		if (!m.pblk || m.unwritten) {
			memset(out + done, 0, n);
		} else if (boff == 0 && n >= fs->bs) {
			/* aligned part straight into the caller's buffer */
			uint32_t nb = (uint32_t)(n / fs->bs);
			r = dev_read_data(fs, m.pblk, nb, out + done);
			if (r) {
				free(bounce);
				return r;
			}
			n = (size_t)nb * fs->bs;
		} else {
			if (!bounce && !(bounce = malloc(fs->bs)))
				return -ENOMEM;
			r = dev_read_data(fs, m.pblk, 1, bounce);
			if (r) {
				free(bounce);
				return r;
			}
			n = L2M_MIN(n, (size_t)(fs->bs - boff));
			memcpy(out + done, bounce + boff, n);
		}
		done += n;
	}
	free(bounce);
	return (int64_t)done;
}

int64_t l2m_read(l2m_fs *fs, uint32_t ino, uint64_t off, void *buf, size_t len)
{
	struct l2m_inode in;
	LOCK(fs);
	int64_t r = read_live_inode(fs, ino, &in);
	if (!r)
		r = read_locked(fs, &in, off, buf, len);
	UNLOCK(fs);
	return r;
}

static int writable_file(struct l2m_inode *in)
{
	if (inode_is_dir(in))
		return -EISDIR;
	if (!inode_is_reg(in) && !inode_is_lnk(in))
		return -EINVAL;
	if (in->i.i_flags & (EXT4_IMMUTABLE_FL | EXT4_VERITY_FL | EXT4_ENCRYPT_FL))
		return -EPERM;
	if (in->i.i_flags & EXT4_INLINE_DATA_FL)
		return -EOPNOTSUPP;
	if (!(in->i.i_flags & EXT4_EXTENTS_FL))
		return -EOPNOTSUPP;
	return 0;
}

/* write one block with partial content, keeping/zeroing the rest */
static int write_partial_block(l2m_fs *fs, const struct l2m_map *m, uint32_t boff, const uint8_t *src, uint32_t n, bool fresh)
{
	uint8_t *b = malloc(fs->bs);
	if (!b)
		return -ENOMEM;
	int r = 0;
	if (fresh)
		memset(b, 0, fs->bs);
	else
		r = dev_read_data(fs, m->pblk, 1, b);
	if (!r) {
		memcpy(b + boff, src, n);
		r = dev_write_data(fs, m->pblk, 1, b);
	}
	free(b);
	return r;
}

static int64_t write_locked(l2m_fs *fs, struct l2m_inode *in, uint64_t off, const void *buf, size_t len)
{
	const uint8_t *src = buf;
	size_t done = 0;
	int r = writable_file(in);
	if (r)
		return r;
	if (in->i.i_flags & EXT4_APPEND_FL && off != inode_size(in))
		return -EPERM;
	if (len == 0)
		return 0;
	if ((off + len + fs->bs - 1) / fs->bs > MAX_FILE_BLOCKS)
		return -EFBIG;
	/* blocks allocated by this call contain old disk data: partial
	 * writes into them must zero-fill instead of read-modify-write */
	uint64_t fresh_lo = 0, fresh_hi = 0;
	while (done < len) {
		uint64_t pos = off + done;
		uint64_t lblk = pos / fs->bs;
		uint32_t boff = (uint32_t)(pos % fs->bs);
		uint64_t want_blocks = L2M_DIV_ROUND_UP(boff + (len - done), fs->bs);
		r = txn_need_room(fs, 16);
		if (r)
			break;
		struct l2m_map m;
		r = bmap(fs, in, lblk, &m);
		if (r)
			break;
		if (!m.pblk) {
			r = bmap_alloc(fs, in, lblk, want_blocks, false, 0, &m);
			if (r == -ENOSPC) {
				/* space freed by recent deletes is held until commit */
				r = inode_write(fs, in);
				if (!r && txn_enospc_retry(fs) == 1)
					r = bmap_alloc(fs, in, lblk, want_blocks, false, 0, &m);
				else if (!r)
					r = -ENOSPC;
			}
			if (r)
				break;
			fresh_lo = m.lblk;
			fresh_hi = m.lblk + m.len;
		}
		bool fresh = lblk >= fresh_lo && lblk < fresh_hi;
		bool was_unwritten = m.unwritten;
		uint64_t span = (uint64_t)m.len * fs->bs - boff;
		size_t n = (size_t)L2M_MIN(span, (uint64_t)(len - done));
		if (boff || n < fs->bs) {
			n = L2M_MIN(n, (size_t)(fs->bs - boff));
			r = write_partial_block(fs, &m, boff, src + done, (uint32_t)n, fresh || was_unwritten);
			if (!r && was_unwritten)
				r = ext_mark_written(fs, in, lblk, 1);
		} else {
			uint32_t nb = (uint32_t)(n / fs->bs);
			r = dev_write_data(fs, m.pblk, nb, src + done);
			n = (size_t)nb * fs->bs;
			if (!r && was_unwritten)
				r = ext_mark_written(fs, in, lblk, nb);
		}
		if (r)
			break;
		done += n;
		/* keep i_size in step so a commit in between stays consistent */
		if (off + done > inode_size(in))
			inode_set_size(in, off + done);
		r = inode_write(fs, in);
		if (r)
			break;
	}
	if (done == 0)
		return r;
	/* clearing set-uid/set-gid on write is the caller's (VFS) policy */
	inode_touch(fs, in, T_MTIME | T_CTIME);
	int w = inode_write(fs, in);
	if (w)
		return w;
	return (int64_t)done;
}

int64_t l2m_write(l2m_fs *fs, uint32_t ino, uint64_t off, const void *buf, size_t len)
{
	struct l2m_inode in;
	LOCK(fs);
	int64_t r = fs_check_writable(fs);
	if (!r)
		r = read_live_inode(fs, ino, &in);
	if (!r)
		r = write_locked(fs, &in, off, buf, len);
	int c = txn_end_op(fs);
	if (r >= 0 && c)
		r = c;
	UNLOCK(fs);
	return r;
}

static int truncate_locked(l2m_fs *fs, struct l2m_inode *in, uint64_t size)
{
	int r = writable_file(in);
	if (r)
		return r;
	if (in->i.i_flags & EXT4_APPEND_FL)
		return -EPERM;
	uint64_t cur = inode_size(in);
	if ((size + fs->bs - 1) / fs->bs > MAX_FILE_BLOCKS)
		return -EFBIG;
	if (size >= cur) {
		inode_set_size(in, size);
		inode_touch(fs, in, T_MTIME | T_CTIME);
		return inode_write(fs, in);
	}
	/* zero the tail of the new last block so growing later shows zeros */
	uint32_t tail = (uint32_t)(size % fs->bs);
	if (tail) {
		struct l2m_map m;
		r = bmap(fs, in, size / fs->bs, &m);
		if (r)
			return r;
		if (m.pblk && !m.unwritten) {
			uint8_t *b = malloc(fs->bs);
			if (!b)
				return -ENOMEM;
			r = dev_read_data(fs, m.pblk, 1, b);
			if (!r) {
				memset(b + tail, 0, fs->bs - tail);
				r = dev_write_data(fs, m.pblk, 1, b);
			}
			free(b);
			if (r)
				return r;
		}
	}
	bool big = inode_blocks512(fs, in) / (fs->bs / 512) > 4096;
	if (big) {
		r = orphan_add(fs, in);
		if (r)
			return r;
	}
	inode_set_size(in, size);
	koio_cut(fs, in->ino, L2M_DIV_ROUND_UP(size, fs->bs), ~0ull, false, 0);
	r = bmap_truncate(fs, in, L2M_DIV_ROUND_UP(size, fs->bs));
	if (!r && big)
		r = orphan_del(fs, in);
	inode_touch(fs, in, T_MTIME | T_CTIME);
	if (!r)
		r = inode_write(fs, in);
	return r;
}

int l2m_truncate(l2m_fs *fs, uint32_t ino, uint64_t size)
{
	struct l2m_inode in;
	LOCK(fs);
	int r = fs_check_writable(fs);
	if (!r)
		r = read_live_inode(fs, ino, &in);
	if (!r)
		r = truncate_locked(fs, &in, size);
	r = end_op(fs, r);
	UNLOCK(fs);
	return r;
}

int l2m_preallocate(l2m_fs *fs, uint32_t ino, uint64_t off, uint64_t len, uint64_t *allocated)
{
	struct l2m_inode in;
	uint64_t total = 0;
	LOCK(fs);
	int r = fs_check_writable(fs);
	if (!r)
		r = read_live_inode(fs, ino, &in);
	if (!r)
		r = writable_file(&in);
	if (!r && len) {
		uint64_t first = off / fs->bs, last = (off + len - 1) / fs->bs;
		if (last >= MAX_FILE_BLOCKS)
			r = -EFBIG;
		for (uint64_t lb = first; !r && lb <= last;) {
			struct l2m_map m;
			r = txn_need_room(fs, 16);
			if (!r)
				r = bmap(fs, &in, lb, &m);
			if (r)
				break;
			if (!m.pblk) {
				r = bmap_alloc(fs, &in, lb, last - lb + 1, true, 0, &m);
				if (r)
					break;
				total += m.len;
				r = inode_write(fs, &in);
			}
			lb += m.len;
		}
	}
	if (allocated)
		*allocated = total * fs->bs;
	r = end_op(fs, r);
	UNLOCK(fs);
	return r;
}

/* ---- kernel-offloaded I/O bookkeeping -----------------------------------------
 *
 * On macOS the kernel maps file ranges for writing (within numbered
 * operations), writes the data straight to the device and then reports each
 * operation done once, with one byte range. That range only summarises the
 * operation: it can span gaps the operation never wrote, and it can miss
 * data that was written - converting exactly the reported range left zero
 * holes in big downloads (unwritten extents whose blocks held the right
 * data). So the core remembers which unwritten blocks it handed out for
 * writing, and to which operation:
 *
 *  - when an operation reports done, everything it was handed is converted,
 *    wherever its reported range lies;
 *  - unwritten blocks that were never handed out (fallocate on Linux) stay
 *    unwritten even inside a reported range: old disk contents never show
 *    up in a file;
 *  - blocks handed out to operation 0 (the kernel writing through mappings
 *    it kept), to an operation that failed, or to one that has not reported
 *    for a minute are converted as soon as a report covers them; once
 *    they are that old and inside the file they are converted anyway, and
 *    all of them when the kernel releases the file;
 *  - read mappings show handed-out blocks as data: the kernel wrote them or
 *    still holds them dirty, so zeros would be wrong.
 *
 * Everything here runs with fs->lock held. */

static int zero_uncovered_edges(l2m_fs *fs, struct l2m_inode *in, uint64_t off, uint64_t len);

static bool koio_old(l2m_fs *fs, const struct koio_range *k, uint64_t now) { return now - k->since_ms >= fs->koio_lost_ms; }

static int koio_add(l2m_fs *fs, uint32_t ino, uint64_t op, uint64_t lblk, uint64_t end)
{
	for (size_t i = 0; i < fs->nkoio; i++) {
		struct koio_range *k = &fs->koio[i];
		if (k->ino == ino && k->op == op && !k->failed && k->lblk <= end && lblk <= k->end) {
			k->lblk = L2M_MIN(k->lblk, lblk);
			k->end = L2M_MAX(k->end, end);
			return 0;
		}
	}
	if (fs->nkoio == fs->capkoio) {
		size_t cap = fs->capkoio ? fs->capkoio * 2 : 64;
		struct koio_range *v = realloc(fs->koio, cap * sizeof(*v));
		if (!v)
			return -ENOMEM;
		fs->koio = v;
		fs->capkoio = cap;
	}
	fs->koio[fs->nkoio++] = (struct koio_range){lblk, end, op, l2m_now_ms(), ino, false};
	return 0;
}

/* Forgets blocks [lblk, end) of ino (of every operation, or only of op).
 * Without memory for a split the entry stays whole, which only means a
 * no-op conversion later. */
static void koio_cut(l2m_fs *fs, uint32_t ino, uint64_t lblk, uint64_t end, bool only_op, uint64_t op)
{
	for (size_t i = 0; i < fs->nkoio;) {
		struct koio_range *k = &fs->koio[i];
		if (k->ino != ino || (only_op && k->op != op) || k->end <= lblk || k->lblk >= end) {
			i++;
		} else if (k->lblk < lblk && k->end > end) {
			struct koio_range tail = *k;
			tail.lblk = end;
			if (fs->nkoio < fs->capkoio) {
				k->end = lblk;
				fs->koio[fs->nkoio++] = tail;
			}
			i++;
		} else if (k->lblk < lblk) {
			k->end = lblk;
			i++;
		} else if (k->end > end) {
			k->lblk = end;
			i++;
		} else {
			fs->koio[i] = fs->koio[--fs->nkoio];
		}
	}
}

/* Is lblk handed out (true; *to = end of that range) or not (*to = start
 * of the next handed-out range of the file, or ~0)? */
static bool koio_find(l2m_fs *fs, uint32_t ino, uint64_t lblk, uint64_t *to)
{
	*to = ~0ull;
	for (size_t i = 0; i < fs->nkoio; i++) {
		struct koio_range *k = &fs->koio[i];
		if (k->ino != ino || k->failed)
			continue;
		if (k->lblk <= lblk && lblk < k->end) {
			*to = k->end;
			return true;
		}
		if (k->lblk > lblk && k->lblk < *to)
			*to = k->lblk;
	}
	return false;
}

static int map_locked(l2m_fs *fs, uint32_t ino, uint64_t op, bool note, uint64_t lblk, uint64_t count, unsigned flags,
		      l2m_extent *out, int nmax)
{
	struct l2m_inode in;
	int n = 0;
	int r = read_live_inode(fs, ino, &in);
	if (!r && (flags & L2M_MAP_ALLOCATE)) {
		r = fs_check_writable(fs);
		if (!r)
			r = writable_file(&in);
	}
	if (!r && count == 0)
		count = 1;
	bool changed = false;
	while (!r && n < nmax && count) {
		struct l2m_map m;
		if (lblk >= MAX_FILE_BLOCKS) {
			r = n ? 0 : -EFBIG;
			break;
		}
		r = bmap(fs, &in, lblk, &m);
		if (r)
			break;
		if (!m.pblk && (flags & L2M_MAP_ALLOCATE)) {
			/* a commit must not see the extent tree ahead of the inode */
			if (changed)
				r = inode_write(fs, &in);
			if (!r)
				r = txn_need_room(fs, 16);
			if (!r)
				r = bmap_alloc(fs, &in, lblk, count, true, 0, &m);
			if (r == -ENOSPC) {
				/* space freed by recent deletes is held until commit */
				r = inode_write(fs, &in);
				if (!r && txn_enospc_retry(fs) == 1)
					r = bmap_alloc(fs, &in, lblk, count, true, 0, &m);
				else if (!r)
					r = -ENOSPC;
			}
			if (r)
				break;
			changed = true;
		}
		uint32_t len = (uint32_t)L2M_MIN((uint64_t)m.len, count);
		uint32_t type = !m.pblk ? L2M_EXT_HOLE : (m.unwritten ? L2M_EXT_UNWRITTEN : L2M_EXT_DATA);
		if (type == L2M_EXT_UNWRITTEN && !(flags & L2M_MAP_ALLOCATE) && fs->nkoio) {
			uint64_t to;
			if (koio_find(fs, ino, lblk, &to))
				type = L2M_EXT_DATA;
			len = (uint32_t)L2M_MIN((uint64_t)len, to - lblk);
		}
		if (type == L2M_EXT_UNWRITTEN && note) {
			r = koio_add(fs, ino, op, lblk, lblk + len);
			if (r)
				break;
		}
		out[n].lblk = lblk;
		out[n].pblk = m.pblk;
		out[n].len = len;
		out[n].type = type;
		n++;
		lblk += len;
		count -= len;
	}
	if (changed) {
		int w = inode_write(fs, &in);
		if (!r)
			r = w;
	}
	if (flags & L2M_MAP_ALLOCATE)
		r = end_op(fs, r);
	return r ? r : n;
}

int l2m_map(l2m_fs *fs, uint32_t ino, uint64_t lblk, uint64_t count, unsigned flags, l2m_extent *out, int nmax)
{
	LOCK(fs);
	int r = map_locked(fs, ino, 0, false, lblk, count, flags, out, nmax);
	UNLOCK(fs);
	return r;
}

int l2m_koio_map(l2m_fs *fs, uint32_t ino, uint64_t op, uint64_t lblk, uint64_t count, unsigned flags, l2m_extent *out, int nmax)
{
	LOCK(fs);
	int r = map_locked(fs, ino, op, (flags & L2M_MAP_ALLOCATE) != 0, lblk, count, flags, out, nmax);
	UNLOCK(fs);
	return r;
}

int l2m_koio_unmap(l2m_fs *fs, uint32_t ino, uint64_t op, uint64_t lblk, uint64_t count)
{
	LOCK(fs);
	koio_cut(fs, ino, lblk, lblk + count, true, op);
	UNLOCK(fs);
	return 0;
}

/* converts [a, b) of in and forgets it */
static int koio_convert(l2m_fs *fs, struct l2m_inode *in, uint64_t a, uint64_t b)
{
	int r = ext_mark_written(fs, in, a, b - a);
	if (!r)
		koio_cut(fs, in->ino, a, b, false, 0);
	return r;
}

struct koio_todo {
	uint64_t a, b;
};

/* converts the collected ranges; returns the number of blocks or -errno */
static int64_t koio_convert_all(l2m_fs *fs, struct l2m_inode *in, struct koio_todo *t, size_t n)
{
	int64_t blocks = 0;
	for (size_t i = 0; i < n; i++) {
		int r = koio_convert(fs, in, t[i].a, t[i].b);
		if (r)
			return r;
		blocks += (int64_t)(t[i].b - t[i].a);
	}
	return blocks;
}

/* Operation op wrote what it was handed (as far as it lies inside the file:
 * beyond the end it stays unwritten), plus - within its report [a, b) -
 * what operation 0, failed or silent operations were handed. *missed:
 * blocks of op that the report left out. */
static int koio_settle(l2m_fs *fs, struct l2m_inode *in, uint64_t op, uint64_t a, uint64_t b, uint64_t now, uint64_t *missed)
{
	uint64_t eof = L2M_DIV_ROUND_UP(inode_size(in), fs->bs);
	struct koio_todo *t = malloc((fs->nkoio + 1) * sizeof(*t));
	if (!t)
		return -ENOMEM;
	size_t n = 0;
	*missed = 0;
	for (size_t i = 0; i < fs->nkoio; i++) {
		struct koio_range *k = &fs->koio[i];
		if (k->ino != in->ino)
			continue;
		if (op && k->op == op && !k->failed && k->lblk < eof) {
			uint64_t end = L2M_MIN(k->end, eof);
			t[n++] = (struct koio_todo){k->lblk, end};
			uint64_t inside = k->lblk < b && a < end ? L2M_MIN(end, b) - L2M_MAX(k->lblk, a) : 0;
			*missed += end - k->lblk - inside;
		} else if ((k->op == 0 || k->failed || koio_old(fs, k, now)) && k->lblk < b && a < k->end) {
			t[n++] = (struct koio_todo){L2M_MAX(k->lblk, a), L2M_MIN(k->end, b)};
		}
	}
	int64_t r = koio_convert_all(fs, in, t, n);
	free(t);
	return r < 0 ? (int)r : 0;
}

/* Converts what the file was handed long ago (or, with all, everything)
 * and still lies inside it: the kernel wrote it but never said so. */
static int koio_sweep(l2m_fs *fs, struct l2m_inode *in, uint64_t now, bool all)
{
	uint64_t eof = L2M_DIV_ROUND_UP(inode_size(in), fs->bs);
	struct koio_todo *t = malloc((fs->nkoio + 1) * sizeof(*t));
	if (!t)
		return -ENOMEM;
	size_t n = 0;
	uint64_t op = 0;
	for (size_t i = 0; i < fs->nkoio; i++) {
		struct koio_range *k = &fs->koio[i];
		if (k->ino == in->ino && !k->failed && (all || koio_old(fs, k, now)) && k->lblk < eof) {
			t[n++] = (struct koio_todo){k->lblk, L2M_MIN(k->end, eof)};
			op = k->op;
		}
	}
	int64_t r = koio_convert_all(fs, in, t, n);
	free(t);
	if (r > 0)
		l2m_log(fs, L2M_LOG_WARN, "inode %u: %lld block(s) written by the kernel without a report (operation %llu and others) "
			"marked as written", in->ino, (long long)r, (unsigned long long)op);
	if (r >= 0 && all)
		koio_cut(fs, in->ino, 0, ~0ull, false, 0);
	return r < 0 ? (int)r : 0;
}

int l2m_koio_done(l2m_fs *fs, uint32_t ino, uint64_t op, uint64_t off, uint64_t len, int ok)
{
	struct l2m_inode in;
	LOCK(fs);
	int r = fs_check_writable(fs);
	if (!r)
		r = read_live_inode(fs, ino, &in);
	if (!r)
		r = writable_file(&in);
	if (!r && !ok) {
		/* nothing of it counts as written, but a later report may cover it */
		for (size_t i = 0; i < fs->nkoio; i++)
			if (fs->koio[i].ino == ino && fs->koio[i].op == op)
				fs->koio[i].failed = true;
		goto out;
	}
	if (!r)
		r = txn_need_room(fs, 64);
	uint64_t now = l2m_now_ms();
	/* the size first: what the operation wrote beyond it stays unwritten */
	if (!r && off + len > inode_size(&in))
		inode_set_size(&in, off + len);
	if (!r && len) {
		uint64_t a = off / fs->bs, b = L2M_DIV_ROUND_UP(off + len, fs->bs), missed = 0;
		/* the kernel writes whole pages; only blocks bigger than a page
		 * (4 KiB at least) can be partly written */
		if (fs->bs > 4096)
			r = zero_uncovered_edges(fs, &in, off, len);
		if (!r)
			r = koio_settle(fs, &in, op, a, b, now, &missed);
		if (!r && missed)
			l2m_log(fs, L2M_LOG_WARN, "inode %u: operation %llu reported blocks %llu-%llu but also wrote %llu block(s) "
				"outside; marked as written too", ino, (unsigned long long)op, (unsigned long long)a,
				(unsigned long long)b - 1, (unsigned long long)missed);
	}
	if (!r) {
		inode_touch(fs, &in, T_MTIME | T_CTIME);
		r = koio_sweep(fs, &in, now, false);
		if (!r)
			r = inode_write(fs, &in);
	}
out:
	r = end_op(fs, r);
	UNLOCK(fs);
	return r;
}

static int koio_release_locked(l2m_fs *fs, uint32_t ino)
{
	struct l2m_inode in;
	int r = 0;
	if (fs->rdonly || read_live_inode(fs, ino, &in) || !inode_is_reg(&in)) {
		koio_cut(fs, ino, 0, ~0ull, false, 0);
		return 0;
	}
	r = txn_need_room(fs, 64);
	if (!r)
		r = koio_sweep(fs, &in, l2m_now_ms(), true);
	if (!r)
		r = inode_write(fs, &in);
	return end_op(fs, r);
}

int koio_release_all_locked(l2m_fs *fs)
{
	int r = 0;
	while (fs->nkoio && !r) {
		uint32_t first = fs->koio[0].ino;
		r = koio_release_locked(fs, first);
		if (!r)
			koio_cut(fs, first, 0, ~0ull, false, 0); /* in case nothing was converted */
	}
	return r;
}

int l2m_koio_release(l2m_fs *fs, uint32_t ino)
{
	LOCK(fs);
	int r = ino ? koio_release_locked(fs, ino) : koio_release_all_locked(fs);
	UNLOCK(fs);
	return r;
}

/* Edge blocks of [off, off+len) that are only partly covered and still
 * unwritten: zero the uncovered bytes (they read as zero until now). */
static int zero_uncovered_edges(l2m_fs *fs, struct l2m_inode *in, uint64_t off, uint64_t len)
{
	uint64_t first = off / fs->bs, last = (off + len - 1) / fs->bs;
	uint64_t edges[2] = {first, last};
	int r = 0;
	for (int k = 0; k < (first == last ? 1 : 2) && !r; k++) {
		uint64_t lb = edges[k];
		uint32_t from = lb == first ? (uint32_t)(off % fs->bs) : 0;
		uint32_t to = lb == last ? (uint32_t)((off + len - 1) % fs->bs) + 1 : fs->bs;
		if (from == 0 && to == fs->bs)
			continue;
		struct l2m_map m;
		r = bmap(fs, in, lb, &m);
		if (r || !m.pblk || !m.unwritten)
			continue;
		uint8_t *b = malloc(fs->bs);
		if (!b)
			return -ENOMEM;
		r = dev_read_data(fs, m.pblk, 1, b);
		if (!r) {
			memset(b, 0, from);
			memset(b + to, 0, fs->bs - to);
			r = dev_write_data(fs, m.pblk, 1, b);
		}
		free(b);
	}
	return r;
}

int l2m_write_done(l2m_fs *fs, uint32_t ino, uint64_t off, uint64_t len, uint64_t new_size)
{
	struct l2m_inode in;
	LOCK(fs);
	int r = fs_check_writable(fs);
	if (!r)
		r = read_live_inode(fs, ino, &in);
	if (!r)
		r = writable_file(&in);
	if (!r && len) {
		uint64_t first = off / fs->bs, last = (off + len - 1) / fs->bs;
		r = zero_uncovered_edges(fs, &in, off, len);
		if (!r)
			r = ext_mark_written(fs, &in, first, last - first + 1);
	}
	if (!r) {
		if (new_size > inode_size(&in))
			inode_set_size(&in, new_size);
		inode_touch(fs, &in, T_MTIME | T_CTIME);
		r = inode_write(fs, &in);
	}
	r = end_op(fs, r);
	UNLOCK(fs);
	return r;
}

int64_t zero_holes_locked(l2m_fs *fs, struct l2m_inode *in, l2m_zero_fn fn, void *ctx)
{
	int64_t total = 0;
	int r = 0;
	uint32_t ino = in->ino;
	if (inode_is_reg(in) && (in->i.i_flags & EXT4_EXTENTS_FL)) {
		uint64_t end = L2M_DIV_ROUND_UP(inode_size(in), fs->bs);
		for (uint64_t lb = 0; lb < end;) {
			struct l2m_map m;
			r = bmap(fs, in, lb, &m);
			if (r)
				break;
			uint64_t len = L2M_MIN((uint64_t)m.len, end - lb);
			uint64_t to;
			bool busy = false;
			if (m.pblk && m.unwritten && fs->nkoio) {
				/* handed out to the kernel: being written right now */
				busy = koio_find(fs, ino, lb, &to);
				len = L2M_MIN(len, to - lb);
			}
			if (m.pblk && m.unwritten && !busy) {
				total += (int64_t)len;
				if (fn && fn(ctx, lb, m.pblk + (lb - m.lblk), (uint32_t)len))
					break;
			}
			lb += len ? len : 1;
		}
	}
	return r ? r : total;
}

int64_t l2m_zero_holes(l2m_fs *fs, uint32_t ino, l2m_zero_fn fn, void *ctx)
{
	struct l2m_inode in;
	LOCK(fs);
	int64_t r = read_live_inode(fs, ino, &in);
	if (!r)
		r = zero_holes_locked(fs, &in, fn, ctx);
	UNLOCK(fs);
	return r;
}

/* ---- paths of inodes ---------------------------------------------------------------- */

struct pathwalk {
	l2m_fs *fs;
	const uint32_t *inos;
	int n;
	l2m_path_fn fn;
	void *ctx;
	char path[4096];
	size_t len;
	uint32_t *dirs; /* subdirectories of the directory being read */
	size_t ndirs, capdirs;
	char **names;
};

static int pathwalk_entry(void *ctx, const char *name, size_t len, uint32_t ino, unsigned dtype, uint64_t next)
{
	(void)next;
	struct pathwalk *w = ctx;
	if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
		return 0;
	if (w->len + 1 + len >= sizeof(w->path))
		return 0;
	for (int i = 0; i < w->n; i++) {
		if (w->inos[i] == ino) {
			char p[4096];
			snprintf(p, sizeof(p), "%.*s/%.*s", (int)w->len, w->path, (int)len, name);
			w->fn(w->ctx, ino, p);
		}
	}
	bool dir = dtype == L2M_DT_DIR;
	if (dtype == 0) {
		l2m_stat st;
		dir = l2m_getattr(w->fs, ino, &st) == 0 && (st.mode & 0170000) == 0040000;
	}
	if (dir) {
		if (w->ndirs == w->capdirs) {
			size_t cap = w->capdirs ? w->capdirs * 2 : 32;
			uint32_t *d = realloc(w->dirs, cap * sizeof(*d));
			char **nm = realloc(w->names, cap * sizeof(*nm));
			if (d)
				w->dirs = d;
			if (nm)
				w->names = nm;
			if (!d || !nm)
				return -ENOMEM;
			w->capdirs = cap;
		}
		w->names[w->ndirs] = strndup(name, len);
		w->dirs[w->ndirs++] = ino;
	}
	return 0;
}

static int pathwalk_dir(struct pathwalk *w, uint32_t dir, int depth)
{
	if (depth > 64)
		return 0;
	size_t first = w->ndirs;
	uint64_t cookie = 0;
	int r = l2m_readdir(w->fs, dir, &cookie, pathwalk_entry, w);
	size_t last = w->ndirs, len = w->len;
	for (size_t i = first; i < last && !r; i++) {
		size_t k = strlen(w->names[i]);
		if (len + 1 + k < sizeof(w->path)) {
			w->path[len] = '/';
			memcpy(w->path + len + 1, w->names[i], k);
			w->len = len + 1 + k;
			r = pathwalk_dir(w, w->dirs[i], depth + 1);
			w->len = len;
		}
	}
	for (size_t i = first; i < last; i++)
		free(w->names[i]);
	w->ndirs = first;
	return r;
}

int l2m_find_paths(l2m_fs *fs, const uint32_t *inos, int n, l2m_path_fn fn, void *ctx)
{
	struct pathwalk w = {.fs = fs, .inos = inos, .n = n, .fn = fn, .ctx = ctx};
	int r = n > 0 ? pathwalk_dir(&w, L2M_ROOT_INO, 0) : 0;
	free(w.dirs);
	free(w.names);
	return r;
}

/* ---- extended attributes --------------------------------------------------------- */

static int xname(const char *name, int *index, const char **suffix)
{
	/* macOS names map to the Linux user namespace */
	static const struct {
		const char *p;
		int idx;
	} ro[] = {{"security.", EXT4_XATTR_INDEX_SECURITY}, {"trusted.", EXT4_XATTR_INDEX_TRUSTED}, {"system.", EXT4_XATTR_INDEX_SYSTEM}};
	if (strncmp(name, "user.", 5) == 0) {
		/* already Linux style */
		*index = EXT4_XATTR_INDEX_USER;
		*suffix = name + 5;
		return 0;
	}
	for (size_t i = 0; i < sizeof(ro) / sizeof(ro[0]); i++)
		if (strncmp(name, ro[i].p, strlen(ro[i].p)) == 0) {
			*index = ro[i].idx;
			*suffix = name + strlen(ro[i].p);
			return 1; /* read-only namespace */
		}
	*index = EXT4_XATTR_INDEX_USER;
	*suffix = name;
	return 0;
}

int64_t l2m_getxattr(l2m_fs *fs, uint32_t ino, const char *name, void *buf, size_t len)
{
	struct l2m_inode in;
	int idx;
	const char *suf;
	xname(name, &idx, &suf);
	LOCK(fs);
	int64_t r = read_live_inode(fs, ino, &in);
	if (!r)
		r = xattr_get(fs, &in, idx, suf, buf, len);
	UNLOCK(fs);
	return r;
}

int l2m_setxattr(l2m_fs *fs, uint32_t ino, const char *name, const void *val, size_t len, unsigned flags)
{
	struct l2m_inode in;
	int idx;
	const char *suf;
	if (xname(name, &idx, &suf))
		return -EPERM;
	if (!val)
		val = "";
	LOCK(fs);
	int r = fs_check_writable(fs);
	if (!r)
		r = read_live_inode(fs, ino, &in);
	if (!r && (in.i.i_flags & EXT4_IMMUTABLE_FL))
		r = -EPERM;
	if (!r)
		r = txn_need_room(fs, 8);
	if (!r)
		r = xattr_set(fs, &in, idx, suf, val, len, flags);
	if (!r) {
		inode_touch(fs, &in, T_CTIME);
		r = inode_write(fs, &in);
	}
	r = end_op(fs, r);
	UNLOCK(fs);
	return r;
}

int l2m_removexattr(l2m_fs *fs, uint32_t ino, const char *name)
{
	struct l2m_inode in;
	int idx;
	const char *suf;
	if (xname(name, &idx, &suf))
		return -EPERM;
	LOCK(fs);
	int r = fs_check_writable(fs);
	if (!r)
		r = read_live_inode(fs, ino, &in);
	if (!r)
		r = txn_need_room(fs, 8);
	if (!r)
		r = xattr_set(fs, &in, idx, suf, NULL, 0, 0);
	if (!r) {
		inode_touch(fs, &in, T_CTIME);
		r = inode_write(fs, &in);
	}
	r = end_op(fs, r);
	UNLOCK(fs);
	return r;
}

int64_t l2m_listxattr(l2m_fs *fs, uint32_t ino, char *buf, size_t len)
{
	struct l2m_inode in;
	LOCK(fs);
	int64_t r = read_live_inode(fs, ino, &in);
	if (!r)
		r = xattr_list(fs, &in, buf, len);
	UNLOCK(fs);
	return r;
}
