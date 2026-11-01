/*
 * ExtBridge - block and inode allocation.
 *
 * Simple, predictable allocator: first fit from a goal block (usually the
 * block behind the previous extent of the file), skipping blocks freed in
 * the running transaction. Large writes therefore get long contiguous
 * extents, which is what makes sequential I/O fast.
 *
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

static inline bool test_bit(const uint8_t *bm, uint32_t i) { return bm[i >> 3] & (1u << (i & 7)); }
static inline void set_bit(uint8_t *bm, uint32_t i) { bm[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline void clear_bit(uint8_t *bm, uint32_t i) { bm[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

/* first zero bit in [from, to) or to */
static uint32_t find_zero(const uint8_t *bm, uint32_t from, uint32_t to)
{
	uint32_t i = from;
	while (i < to && (i & 63)) {
		if (!test_bit(bm, i))
			return i;
		i++;
	}
	while (i + 64 <= to) {
		uint64_t w;
		memcpy(&w, bm + (i >> 3), 8);
		if (w != UINT64_MAX)
			return i + (uint32_t)__builtin_ctzll(~w);
		i += 64;
	}
	while (i < to) {
		if (!test_bit(bm, i))
			return i;
		i++;
	}
	return to;
}

static uint64_t reserved_blocks(l2m_fs *fs)
{
	return fs->sb->s_r_blocks_count_lo | (fs->is64 ? (uint64_t)fs->sb->s_r_blocks_count_hi << 32 : 0);
}

static int alloc_in_group(l2m_fs *fs, uint32_t g, uint32_t from, uint32_t want, uint64_t *start, uint32_t *got)
{
	struct ext4_group_desc *gd = gd_get(fs, g);
	if (gd_free_blocks(fs, gd) == 0)
		return -ENOSPC;
	struct l2m_buf *b;
	int r = block_bitmap_get(fs, g, &b, false);
	if (r)
		return r;
	uint32_t n = group_nblocks(fs, g);
	uint64_t base = group_first_block(fs, g);
	uint32_t i = from < n ? from : 0;
	int wrapped = from == 0;
	for (;;) {
		i = find_zero(b->data, i, n);
		if (i >= n) {
			if (wrapped)
				break;
			wrapped = 1;
			n = L2M_MIN(from, group_nblocks(fs, g));
			i = 0;
			continue;
		}
		uint64_t next_ok;
		if (txn_is_freed(fs, base + i, &next_ok)) {
			i = (uint32_t)L2M_MIN(next_ok - base, (uint64_t)n);
			continue;
		}
		/* found a free block: extend the run */
		uint32_t k = i + 1;
		uint32_t lim = group_nblocks(fs, g);
		while (k < lim && k - i < want && !test_bit(b->data, k) && !txn_is_freed(fs, base + k, NULL))
			k++;
		buf_put(fs, b);
		/* now take it for writing (initialises BLOCK_UNINIT groups) */
		r = block_bitmap_get(fs, g, &b, true);
		if (r)
			return r;
		for (uint32_t m = i; m < k; m++) {
			if (test_bit(b->data, m)) {
				buf_put(fs, b);
				return l2m_corrupt(fs, "allocator found a used block", base + m);
			}
			set_bit(b->data, m);
		}
		r = buf_dirty(fs, b);
		if (!r) {
			gd_set_free_blocks(fs, gd, gd_free_blocks(fs, gd) - (k - i));
			r = block_bitmap_update_csum(fs, g, b);
		}
		buf_put(fs, b);
		if (r)
			return r;
		sb_add_free_blocks(fs, -(int64_t)(k - i));
		*start = base + i;
		*got = k - i;
		fs->last_alloc_group = g;
		return 0;
	}
	buf_put(fs, b);
	return -ENOSPC;
}

int alloc_blocks(l2m_fs *fs, uint64_t goal, uint32_t want, uint64_t *start, uint32_t *got, bool metadata)
{
	int r = fs_check_writable(fs);
	if (r)
		return r;
	if (want == 0)
		want = 1;
	uint64_t free = sb_free_blocks(fs), resv = reserved_blocks(fs);
	if (free == 0 || (!metadata && free <= resv))
		return -ENOSPC;
	if (!metadata && free - resv < want)
		want = (uint32_t)(free - resv);
	if (goal < fs->first_data_block || goal >= fs->blocks_count)
		goal = fs->first_data_block;
	uint32_t g0 = (uint32_t)((goal - fs->first_data_block) / fs->bpg);
	uint32_t off = (uint32_t)((goal - fs->first_data_block) % fs->bpg);
	for (uint32_t k = 0; k < fs->groups; k++) {
		uint32_t g = (g0 + k) % fs->groups;
		r = alloc_in_group(fs, g, k == 0 ? off : 0, want, start, got);
		if (r == -ENOSPC)
			continue;
		return r;
	}
	return -ENOSPC;
}

int free_blocks(l2m_fs *fs, uint64_t start, uint64_t count)
{
	int r = fs_check_writable(fs);
	if (r)
		return r;
	if (start < fs->first_data_block || start + count > fs->blocks_count || count == 0)
		return l2m_corrupt(fs, "freeing blocks out of range", start);
	while (count) {
		uint32_t g = (uint32_t)((start - fs->first_data_block) / fs->bpg);
		uint64_t base = group_first_block(fs, g);
		uint32_t i = (uint32_t)(start - base);
		uint32_t n = (uint32_t)L2M_MIN(count, (uint64_t)(group_nblocks(fs, g) - i));
		struct l2m_buf *b;
		r = block_bitmap_get(fs, g, &b, true);
		if (r)
			return r;
		for (uint32_t m = i; m < i + n; m++) {
			if (!test_bit(b->data, m)) {
				buf_put(fs, b);
				return l2m_corrupt(fs, "freeing a block that is already free", base + m);
			}
			clear_bit(b->data, m);
		}
		r = buf_dirty(fs, b);
		struct ext4_group_desc *gd = gd_get(fs, g);
		if (!r) {
			gd_set_free_blocks(fs, gd, gd_free_blocks(fs, gd) + n);
			r = block_bitmap_update_csum(fs, g, b);
		}
		buf_put(fs, b);
		if (r)
			return r;
		sb_add_free_blocks(fs, n);
		r = txn_add_freed(fs, start, n);
		if (r)
			return r;
		start += n;
		count -= n;
	}
	return 0;
}

bool block_in_use(l2m_fs *fs, uint64_t blk, int *err)
{
	uint32_t g = (uint32_t)((blk - fs->first_data_block) / fs->bpg);
	struct l2m_buf *b;
	*err = block_bitmap_get(fs, g, &b, false);
	if (*err)
		return true;
	bool used = test_bit(b->data, (uint32_t)(blk - group_first_block(fs, g)));
	buf_put(fs, b);
	return used;
}

static int alloc_inode_in_group(l2m_fs *fs, uint32_t g, bool is_dir, uint32_t *ino)
{
	struct ext4_group_desc *gd = gd_get(fs, g);
	if (gd_free_inodes(fs, gd) == 0)
		return -ENOSPC;
	struct l2m_buf *b;
	int r = inode_bitmap_get(fs, g, &b, true);
	if (r)
		return r;
	uint32_t start = 0;
	if (g == 0 && fs->first_ino > 1)
		start = fs->first_ino - 1;
	uint32_t i = find_zero(b->data, start, fs->ipg);
	if (i >= fs->ipg) {
		buf_put(fs, b);
		l2m_log(fs, L2M_LOG_WARN, "group %u: free inode count says free but bitmap is full", g);
		return -ENOSPC;
	}
	set_bit(b->data, i);
	r = buf_dirty(fs, b);
	if (!r) {
		gd_set_free_inodes(fs, gd, gd_free_inodes(fs, gd) - 1);
		if (is_dir)
			gd_set_used_dirs(fs, gd, gd_used_dirs(fs, gd) + 1);
		if (fs->has_csum || fs->has_gdt_csum) {
			uint32_t unused = gd_itable_unused(fs, gd);
			if (i + 1 > fs->ipg - unused)
				gd_set_itable_unused(fs, gd, fs->ipg - (i + 1));
		}
		r = inode_bitmap_update_csum(fs, g, b);
	}
	buf_put(fs, b);
	if (r)
		return r;
	sb_add_free_inodes(fs, -1);
	*ino = g * fs->ipg + i + 1;
	return 0;
}

int alloc_inode(l2m_fs *fs, uint32_t parent, bool is_dir, uint32_t *ino)
{
	int r = fs_check_writable(fs);
	if (r)
		return r;
	if (fs->sb->s_free_inodes_count == 0)
		return -ENOSPC;
	uint32_t pg = parent ? (parent - 1) / fs->ipg : 0;
	if (is_dir && parent == L2M_ROOT_INO && fs->groups > 1) {
		/* spread top level directories: group with the most free blocks
		 * among those with at least average free inodes */
		uint32_t avg = fs->sb->s_free_inodes_count / fs->groups, best = pg;
		uint32_t bestfree = 0;
		for (uint32_t k = 0; k < fs->groups; k++) {
			struct ext4_group_desc *gd = gd_get(fs, k);
			uint32_t fi = gd_free_inodes(fs, gd), fb = gd_free_blocks(fs, gd);
			if (fi && fi >= avg && fb > bestfree)
				best = k, bestfree = fb;
		}
		pg = best;
	}
	for (uint32_t k = 0; k < fs->groups; k++) {
		uint32_t g = (pg + k) % fs->groups;
		r = alloc_inode_in_group(fs, g, is_dir, ino);
		if (r == -ENOSPC)
			continue;
		return r;
	}
	return -ENOSPC;
}

bool inode_allocated(l2m_fs *fs, uint32_t ino, int *err)
{
	*err = 0;
	if (ino == 0 || ino > fs->sb->s_inodes_count)
		return false;
	uint32_t g = (ino - 1) / fs->ipg, i = (ino - 1) % fs->ipg;
	struct l2m_buf *b;
	*err = inode_bitmap_get(fs, g, &b, false);
	if (*err)
		return true; /* unknown: be conservative */
	bool used = test_bit(b->data, i);
	buf_put(fs, b);
	return used;
}

int free_inode(l2m_fs *fs, uint32_t ino, bool is_dir)
{
	int r = fs_check_writable(fs);
	if (r)
		return r;
	if (ino < fs->first_ino || ino > fs->sb->s_inodes_count)
		return l2m_corrupt(fs, "freeing invalid inode", ino);
	uint32_t g = (ino - 1) / fs->ipg, i = (ino - 1) % fs->ipg;
	struct l2m_buf *b;
	r = inode_bitmap_get(fs, g, &b, true);
	if (r)
		return r;
	if (!test_bit(b->data, i)) {
		buf_put(fs, b);
		return l2m_corrupt(fs, "freeing an inode that is already free", ino);
	}
	clear_bit(b->data, i);
	r = buf_dirty(fs, b);
	struct ext4_group_desc *gd = gd_get(fs, g);
	if (!r) {
		gd_set_free_inodes(fs, gd, gd_free_inodes(fs, gd) + 1);
		if (is_dir)
			gd_set_used_dirs(fs, gd, gd_used_dirs(fs, gd) - 1);
		r = inode_bitmap_update_csum(fs, g, b);
	}
	buf_put(fs, b);
	if (r)
		return r;
	sb_add_free_inodes(fs, 1);
	return 0;
}
