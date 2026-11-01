/*
 * ExtBridge - inode table access, inode checksums, timestamps.
 *
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

int inode_location(l2m_fs *fs, uint32_t ino, uint64_t *blk, uint32_t *off)
{
	if (ino == 0 || ino > fs->sb->s_inodes_count)
		return l2m_corrupt(fs, "inode number out of range", ino);
	uint32_t g = (ino - 1) / fs->ipg, idx = (ino - 1) % fs->ipg;
	struct ext4_group_desc *gd = gd_get(fs, g);
	uint64_t table = gd_inode_table(fs, gd);
	uint64_t byte = (uint64_t)idx * fs->inode_size;
	*blk = table + byte / fs->bs;
	*off = (uint32_t)(byte % fs->bs);
	return 0;
}

bool inode_has_extra(l2m_fs *fs, const struct l2m_inode *in, size_t field_end)
{
	if (fs->inode_size <= EXT4_GOOD_OLD_INODE_SIZE)
		return false;
	return EXT4_GOOD_OLD_INODE_SIZE + (size_t)in->i.i_extra_isize >= field_end;
}

void inode_init_seed(l2m_fs *fs, struct l2m_inode *in)
{
	if (!fs->has_csum)
		return;
	uint32_t ino = in->ino, gen = in->i.i_generation;
	uint32_t c = l2m_crc32c(fs->csum_seed, &ino, 4);
	in->csum_seed = l2m_crc32c(c, &gen, 4);
}

uint32_t inode_compute_csum(l2m_fs *fs, struct l2m_inode *in)
{
	uint8_t tmp[L2M_MAX_INODE_SIZE];
	memcpy(tmp, in->raw, fs->inode_size);
	struct ext4_inode *t = (struct ext4_inode *)tmp;
	t->i_checksum_lo = 0;
	bool hi = fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE &&
		  in->i.i_extra_isize >= EXT4_INODE_CSUM_HI_EXTRA_END;
	if (hi)
		t->i_checksum_hi = 0;
	return l2m_crc32c(in->csum_seed, tmp, fs->inode_size);
}

int inode_read(l2m_fs *fs, uint32_t ino, struct l2m_inode *in)
{
	uint64_t blk;
	uint32_t off;
	int r = inode_location(fs, ino, &blk, &off);
	if (r)
		return r;
	/* inodes beyond itable_unused were never initialised */
	uint32_t g = (ino - 1) / fs->ipg;
	struct ext4_group_desc *gd = gd_get(fs, g);
	if ((fs->has_csum || fs->has_gdt_csum) && (gd->bg_flags & EXT4_BG_INODE_UNINIT)) {
		memset(in, 0, sizeof(*in));
		in->ino = ino;
		return -ENOENT;
	}
	struct l2m_buf *b;
	r = buf_get(fs, blk, &b);
	if (r)
		return r;
	memset(in, 0, sizeof(*in));
	in->ino = ino;
	memcpy(in->raw, b->data + off, fs->inode_size);
	buf_put(fs, b);
	if (fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE &&
	    (in->i.i_extra_isize > fs->inode_size - EXT4_GOOD_OLD_INODE_SIZE || (in->i.i_extra_isize & 3)))
		return l2m_corrupt(fs, "inode has invalid extra_isize", ino);
	inode_init_seed(fs, in);
	if (fs->has_csum && fs->verify_csums) {
		/* an all-zero inode (never used) has no checksum */
		bool zero = true;
		for (uint32_t i = 0; i < fs->inode_size && zero; i++)
			zero = in->raw[i] == 0;
		if (!zero) {
			uint32_t c = inode_compute_csum(fs, in);
			uint32_t want = in->i.i_checksum_lo;
			bool hi = fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE &&
				  in->i.i_extra_isize >= EXT4_INODE_CSUM_HI_EXTRA_END;
			if (hi)
				want |= (uint32_t)in->i.i_checksum_hi << 16;
			else
				c &= 0xFFFF;
			if (c != want) {
				l2m_log(fs, L2M_LOG_ERROR, "inode %u checksum mismatch", ino);
				return -EFSBADCRC;
			}
		}
	}
	return 0;
}

int inode_write(l2m_fs *fs, struct l2m_inode *in)
{
	uint64_t blk;
	uint32_t off;
	int r = fs_check_writable(fs);
	if (r)
		return r;
	r = inode_location(fs, in->ino, &blk, &off);
	if (r)
		return r;
	if (fs->has_csum) {
		inode_init_seed(fs, in);
		uint32_t c = inode_compute_csum(fs, in);
		in->i.i_checksum_lo = (uint16_t)c;
		if (fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE && in->i.i_extra_isize >= EXT4_INODE_CSUM_HI_EXTRA_END)
			in->i.i_checksum_hi = (uint16_t)(c >> 16);
	}
	struct l2m_buf *b;
	r = buf_get(fs, blk, &b);
	if (r)
		return r;
	memcpy(b->data + off, in->raw, fs->inode_size);
	r = buf_dirty(fs, b);
	buf_put(fs, b);
	return r;
}

uint64_t inode_size(const struct l2m_inode *in)
{
	return in->i.i_size_lo | ((uint64_t)in->i.i_size_high << 32);
}

void inode_set_size(struct l2m_inode *in, uint64_t size)
{
	in->i.i_size_lo = (uint32_t)size;
	in->i.i_size_high = (uint32_t)(size >> 32);
}

uint64_t inode_blocks512(l2m_fs *fs, const struct l2m_inode *in)
{
	uint64_t b = in->i.i_blocks_lo;
	if (fs->has_huge_file) {
		b |= (uint64_t)in->i.i_blocks_high << 32;
		if (in->i.i_flags & EXT4_HUGE_FILE_FL)
			b <<= (fs->log_bs - 9);
	}
	return b;
}

void inode_add_blocks(l2m_fs *fs, struct l2m_inode *in, int64_t fs_blocks)
{
	uint64_t cur = inode_blocks512(fs, in);
	int64_t delta = fs_blocks * ((int64_t)1 << (fs->log_bs - 9));
	uint64_t v = (uint64_t)((int64_t)cur + delta);
	if ((int64_t)cur + delta < 0)
		v = 0;
	in->i.i_flags &= ~(uint32_t)EXT4_HUGE_FILE_FL;
	in->i.i_blocks_lo = (uint32_t)v;
	if (fs->has_huge_file)
		in->i.i_blocks_high = (uint16_t)(v >> 32);
}

uint32_t inode_uid(const struct l2m_inode *in) { return in->i.i_uid | ((uint32_t)in->i.i_uid_high << 16); }
uint32_t inode_gid(const struct l2m_inode *in) { return in->i.i_gid | ((uint32_t)in->i.i_gid_high << 16); }

uint64_t inode_file_acl(const struct l2m_inode *in)
{
	return in->i.i_file_acl_lo | ((uint64_t)in->i.i_file_acl_high << 32);
}

void inode_set_file_acl(struct l2m_inode *in, uint64_t blk)
{
	in->i.i_file_acl_lo = (uint32_t)blk;
	in->i.i_file_acl_high = (uint16_t)(blk >> 32);
}

static void decode_time(uint32_t lo, const uint32_t *extra, l2m_time *t)
{
	t->sec = (int32_t)lo;
	t->nsec = 0;
	if (extra) {
		uint32_t e = *extra;
		if (e & 3)
			t->sec += (int64_t)(e & 3) << 32;
		t->nsec = e >> 2;
		if (t->nsec > 999999999)
			t->nsec = 0;
	}
}

static uint32_t encode_extra(const l2m_time *t)
{
	uint32_t epoch = (uint32_t)(((t->sec - (int32_t)t->sec) >> 32) & 3);
	return epoch | (t->nsec << 2);
}

#define FEND(f) (offsetof(struct ext4_inode, f) + sizeof(((struct ext4_inode *)0)->f))

void inode_get_time(const struct l2m_inode *in, int which, l2m_time *t)
{
	/* inode_has_extra() needs fs; the extra_isize check alone is enough
	 * because inode_read() rejected impossible values */
	size_t avail = EXT4_GOOD_OLD_INODE_SIZE + in->i.i_extra_isize;
	if (in->i.i_extra_isize == 0)
		avail = 0;
	switch (which) {
	case T_ATIME:
		decode_time(in->i.i_atime, avail >= FEND(i_atime_extra) ? &in->i.i_atime_extra : NULL, t);
		break;
	case T_MTIME:
		decode_time(in->i.i_mtime, avail >= FEND(i_mtime_extra) ? &in->i.i_mtime_extra : NULL, t);
		break;
	case T_CTIME:
		decode_time(in->i.i_ctime, avail >= FEND(i_ctime_extra) ? &in->i.i_ctime_extra : NULL, t);
		break;
	case T_CRTIME:
		if (avail >= FEND(i_crtime))
			decode_time(in->i.i_crtime, avail >= FEND(i_crtime_extra) ? &in->i.i_crtime_extra : NULL, t);
		else
			decode_time(in->i.i_ctime, NULL, t);
		break;
	}
}

void inode_set_time(l2m_fs *fs, struct l2m_inode *in, int which, const l2m_time *t)
{
	uint32_t lo = (uint32_t)t->sec, ex = encode_extra(t);
	switch (which) {
	case T_ATIME:
		in->i.i_atime = lo;
		if (inode_has_extra(fs, in, FEND(i_atime_extra)))
			in->i.i_atime_extra = ex;
		break;
	case T_MTIME:
		in->i.i_mtime = lo;
		if (inode_has_extra(fs, in, FEND(i_mtime_extra)))
			in->i.i_mtime_extra = ex;
		break;
	case T_CTIME:
		in->i.i_ctime = lo;
		if (inode_has_extra(fs, in, FEND(i_ctime_extra)))
			in->i.i_ctime_extra = ex;
		break;
	case T_CRTIME:
		if (inode_has_extra(fs, in, FEND(i_crtime)))
			in->i.i_crtime = lo;
		if (inode_has_extra(fs, in, FEND(i_crtime_extra)))
			in->i.i_crtime_extra = ex;
		break;
	}
}

void inode_touch(l2m_fs *fs, struct l2m_inode *in, int mask)
{
	l2m_time now;
	l2m_now(&now);
	for (int w = 1; w <= 8; w <<= 1)
		if (mask & w)
			inode_set_time(fs, in, w, &now);
}

bool inode_is_fast_symlink(l2m_fs *fs, const struct l2m_inode *in)
{
	if (!inode_is_lnk(in))
		return false;
	if (in->i.i_flags & (EXT4_EXTENTS_FL | EXT4_INLINE_DATA_FL))
		return false;
	uint64_t sz = inode_size(in);
	if (sz == 0 || sz >= sizeof(in->i.i_block))
		return false;
	/* old kernels: fast if no data blocks apart from an xattr block */
	uint64_t blocks = inode_blocks512(fs, in);
	uint64_t ea = inode_file_acl(in) ? (fs->bs >> 9) : 0;
	return blocks <= ea;
}
