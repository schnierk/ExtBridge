/*
 * ExtBridge - read-only consistency scan for the diagnostics view.
 *
 * This is not a replacement for e2fsck (which can repair); it checks the
 * things that can be verified quickly without writing: checksums of all
 * metadata, free counters against the bitmaps, the extent trees of all
 * used inodes, and every used block against the block bitmaps.
 *
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

struct rep {
	char *buf;
	size_t len, used;
	int problems;
};

static void say(struct rep *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(struct rep *r, const char *fmt, ...)
{
	if (!r->buf || r->used + 2 >= r->len)
		return;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(r->buf + r->used, r->len - r->used, fmt, ap);
	va_end(ap);
	if (n > 0)
		r->used = L2M_MIN(r->used + (size_t)n, r->len - 1);
}

static void problem(struct rep *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void problem(struct rep *r, const char *fmt, ...)
{
	char tmp[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	r->problems++;
	if (r->problems <= 200)
		say(r, "PROBLEM: %s\n", tmp);
	else if (r->problems == 201)
		say(r, "... further problems not listed\n");
}

static void say_line(void *ctx, const char *line)
{
	say(ctx, "%s\n", line);
}

/* ---- cross-check: blocks really in use against the block bitmaps --------
 *
 * Like e2fsck's pass 5: every block the group metadata, the journal and the
 * files use must be marked in the bitmaps. A used block that the bitmap
 * calls free is dangerous (the allocator may hand it out again); a marked
 * block nobody uses only wastes space. Needs one bit per block in memory. */

struct xc {
	l2m_fs *fs;
	uint8_t *used;
	uint64_t dups, bad_range;
	uint32_t ino;
	struct rep *rp;
	/* blocks in use but free in the bitmap, as sorted ranges */
	uint64_t (*miss)[2];
	size_t nmiss, capmiss;
	uint64_t missing, leaked;
	uint64_t hits; /* per inode, second walk */
	uint64_t *xattr;
	size_t nxattr, capxattr;
};

static inline bool bit_get(const uint8_t *bm, uint64_t i) { return (bm[i >> 3] >> (i & 7)) & 1; }
static inline void bit_set(uint8_t *bm, uint64_t i) { bm[i >> 3] |= (uint8_t)(1u << (i & 7)); }

static int xc_mark(void *ctx, uint64_t pblk, uint64_t len, bool meta)
{
	(void)meta;
	struct xc *x = ctx;
	if (pblk < x->fs->first_data_block || pblk + len > x->fs->blocks_count || pblk + len < pblk) {
		x->bad_range++;
		return 0;
	}
	for (uint64_t b = pblk; b < pblk + len; b++) {
		if (bit_get(x->used, b)) {
			if (++x->dups <= 20)
				problem(x->rp, "block %llu is used twice (again by inode %u)", (unsigned long long)b, x->ino);
		} else {
			bit_set(x->used, b);
		}
	}
	return 0;
}

static int xc_hit(void *ctx, uint64_t pblk, uint64_t len, bool meta)
{
	(void)meta;
	struct xc *x = ctx;
	/* binary search for the first range ending after pblk */
	size_t lo = 0, hi = x->nmiss;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (x->miss[mid][0] + x->miss[mid][1] <= pblk)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (size_t i = lo; i < x->nmiss && x->miss[i][0] < pblk + len; i++) {
		uint64_t a = L2M_MAX(pblk, x->miss[i][0]), b = L2M_MIN(pblk + len, x->miss[i][0] + x->miss[i][1]);
		if (b > a)
			x->hits += b - a;
	}
	return 0;
}

/* Block-mapped (ext2/3 style) files: depth 0 = the entries are data. */
static int walk_ind(l2m_fs *fs, uint64_t blk, int depth, ext_walk_fn fn, void *ctx)
{
	int r = fn(ctx, blk, 1, true);
	if (r || blk < fs->first_data_block || blk >= fs->blocks_count)
		return r;
	struct l2m_buf *b;
	r = buf_get(fs, blk, &b);
	if (r)
		return r;
	const uint32_t *e = (const uint32_t *)b->data;
	for (uint32_t i = 0; i < fs->bs / 4 && !r; i++) {
		if (!e[i])
			continue;
		r = depth ? walk_ind(fs, e[i], depth - 1, fn, ctx) : fn(ctx, e[i], 1, false);
	}
	buf_put(fs, b);
	return r;
}

/* Every block an inode owns (not its xattr block). */
static int inode_blocks(l2m_fs *fs, struct l2m_inode *in, ext_walk_fn fn, void *ctx)
{
	uint32_t t = in->i.i_mode & L2M_S_IFMT;
	bool special = in->ino < fs->first_ino;
	if (!special && t != L2M_S_IFREG && t != L2M_S_IFDIR && t != L2M_S_IFLNK)
		return 0; /* devices keep numbers, not blocks, in i_block */
	if (in->i.i_flags & EXT4_INLINE_DATA_FL)
		return 0;
	if (t == L2M_S_IFLNK && inode_is_fast_symlink(fs, in))
		return 0;
	if (in->i.i_flags & EXT4_EXTENTS_FL)
		return ext_walk(fs, in, fn, ctx);
	if (in->ino == 7) /* resize inode: its descriptor blocks are group metadata */
		return in->i.i_block[13] ? fn(ctx, in->i.i_block[13], 1, true) : 0;
	int r = 0;
	for (int i = 0; i < 12 && !r; i++)
		if (in->i.i_block[i])
			r = fn(ctx, in->i.i_block[i], 1, false);
	for (int lvl = 0; lvl < 3 && !r; lvl++)
		if (in->i.i_block[12 + lvl])
			r = walk_ind(fs, in->i.i_block[12 + lvl], lvl, fn, ctx);
	return r;
}

typedef int (*inode_fn)(struct xc *x, struct l2m_inode *in);

static void for_each_inode(struct xc *x, inode_fn fn)
{
	l2m_fs *fs = x->fs;
	uint64_t n = 0;
	for (uint32_t g = 0; g < fs->groups; g++) {
		struct ext4_group_desc *gd = gd_get(fs, g);
		if ((fs->has_csum || fs->has_gdt_csum) && (gd->bg_flags & EXT4_BG_INODE_UNINIT))
			continue;
		struct l2m_buf *b;
		if (inode_bitmap_get(fs, g, &b, false))
			continue;
		uint32_t limit = fs->ipg;
		if (fs->has_csum || fs->has_gdt_csum)
			limit = fs->ipg - gd_itable_unused(fs, gd);
		for (uint32_t i = 0; i < limit; i++) {
			if (!(b->data[i >> 3] & (1u << (i & 7))))
				continue;
			struct l2m_inode in;
			x->ino = g * fs->ipg + i + 1;
			if (inode_read(fs, x->ino, &in) == 0)
				fn(x, &in);
			if ((++n & 1023) == 0)
				cache_invalidate_clean(fs);
		}
		buf_put(fs, b);
	}
}

static int xc_inode_mark(struct xc *x, struct l2m_inode *in)
{
	inode_blocks(x->fs, in, xc_mark, x);
	uint64_t acl = inode_file_acl(in);
	if (acl) {
		if (x->nxattr == x->capxattr) {
			size_t nc = x->capxattr ? x->capxattr * 2 : 1024;
			uint64_t *p = realloc(x->xattr, nc * sizeof(*p));
			if (!p)
				return 0;
			x->xattr = p;
			x->capxattr = nc;
		}
		x->xattr[x->nxattr++] = acl;
	}
	return 0;
}

static int xc_inode_hits(struct xc *x, struct l2m_inode *in)
{
	x->hits = 0;
	inode_blocks(x->fs, in, xc_hit, x);
	uint64_t acl = inode_file_acl(in);
	if (acl)
		xc_hit(x, acl, 1, true);
	if (x->hits && x->rp->problems < 200)
		problem(x->rp, "inode %u: %llu of its blocks are marked free in the bitmap", x->ino, (unsigned long long)x->hits);
	return 0;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

static void cross_check(l2m_fs *fs, struct rep *rp)
{
	if (fs->sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_BIGALLOC || fs->has_meta_bg) {
		say(rp, "note: block cross-check not done (bigalloc/meta_bg)\n");
		return;
	}
	struct xc x = {.fs = fs, .rp = rp};
	x.used = calloc((size_t)((fs->blocks_count + 7) / 8), 1);
	if (!x.used) {
		say(rp, "note: block cross-check skipped (not enough memory)\n");
		return;
	}
	/* group metadata */
	x.ino = 0;
	for (uint32_t g = 0; g < fs->groups; g++) {
		struct ext4_group_desc *gd = gd_get(fs, g);
		uint32_t ov = group_super_overhead(fs, g);
		if (ov)
			xc_mark(&x, group_first_block(fs, g), ov, true);
		xc_mark(&x, gd_block_bitmap(fs, gd), 1, true);
		xc_mark(&x, gd_inode_bitmap(fs, gd), 1, true);
		xc_mark(&x, gd_inode_table(fs, gd), fs->inode_table_blocks, true);
	}
	/* files, directories, journal */
	for_each_inode(&x, xc_inode_mark);
	/* shared xattr blocks count once */
	if (x.nxattr)
		qsort(x.xattr, x.nxattr, sizeof(*x.xattr), cmp_u64);
	x.ino = 0;
	for (size_t i = 0; i < x.nxattr; i++)
		if (i == 0 || x.xattr[i] != x.xattr[i - 1])
			xc_mark(&x, x.xattr[i], 1, true);
	/* compare with the bitmaps */
	for (uint32_t g = 0; g < fs->groups; g++) {
		struct l2m_buf *b;
		if (block_bitmap_get(fs, g, &b, false))
			continue;
		uint64_t first = group_first_block(fs, g);
		uint32_t n = group_nblocks(fs, g);
		for (uint32_t i = 0; i < n; i++) {
			bool u = bit_get(x.used, first + i), m = bit_get(b->data, i);
			if (u && !m) {
				x.missing++;
				if (x.nmiss && x.miss[x.nmiss - 1][0] + x.miss[x.nmiss - 1][1] == first + i) {
					x.miss[x.nmiss - 1][1]++;
				} else {
					if (x.nmiss == x.capmiss) {
						size_t nc = x.capmiss ? x.capmiss * 2 : 256;
						void *p = realloc(x.miss, nc * sizeof(*x.miss));
						if (!p)
							break;
						x.miss = p;
						x.capmiss = nc;
					}
					x.miss[x.nmiss][0] = first + i;
					x.miss[x.nmiss][1] = 1;
					x.nmiss++;
				}
			} else if (!u && m) {
				x.leaked++;
			}
		}
		buf_put(fs, b);
	}
	if (x.bad_range)
		problem(rp, "%llu block references point outside the file system", (unsigned long long)x.bad_range);
	if (x.missing) {
		problem(rp, "%llu blocks are in use but marked free in the bitmaps (%zu ranges)", (unsigned long long)x.missing, x.nmiss);
		for (size_t i = 0; i < x.nmiss && i < 10; i++)
			say(rp, "  free in bitmap but used: %llu-%llu\n", (unsigned long long)x.miss[i][0],
			    (unsigned long long)(x.miss[i][0] + x.miss[i][1] - 1));
		for_each_inode(&x, xc_inode_hits);
	}
	if (x.dups > 20)
		say(rp, "  ... %llu blocks used twice in total\n", (unsigned long long)x.dups);
	if (x.leaked)
		say(rp, "note: %llu blocks are marked used but nothing uses them (lost space, harmless)\n", (unsigned long long)x.leaked);
	if (!x.missing && !x.dups && !x.bad_range)
		say(rp, "block cross-check: every used block is marked in the bitmaps\n");
	free(x.used);
	free(x.miss);
	free(x.xattr);
	cache_invalidate_clean(fs);
}

static uint32_t popcount_bits(const uint8_t *bm, uint32_t nbits)
{
	uint32_t c = 0;
	for (uint32_t i = 0; i < nbits / 8; i++)
		c += (uint32_t)__builtin_popcount(bm[i]);
	for (uint32_t i = nbits & ~7u; i < nbits; i++)
		c += (bm[i >> 3] >> (i & 7)) & 1;
	return c;
}

int l2m_verify(l2m_fs *fs, l2m_progress_fn progress, void *ctx, char *report, size_t reportlen)
{
	return l2m_verify_ex(fs, progress, ctx, report, reportlen, NULL);
}

int l2m_verify_ex(l2m_fs *fs, l2m_progress_fn progress, void *ctx, char *report, size_t reportlen, l2m_verify_stats *stats)
{
	struct rep rp = {report, reportlen, 0, 0};
	l2m_verify_stats zh = {0};
	if (report && reportlen)
		report[0] = 0;
	pthread_mutex_lock(&fs->lock);
	int saved = fs->verify_csums;
	fs->verify_csums = 1;
	uint64_t free_blocks = 0, free_inodes = 0, used_inodes_total = 0;
	uint64_t total = (uint64_t)fs->groups * 2;
	uint64_t done = 0;
	int cancelled = 0;

	say(&rp, "ExtBridge %s consistency scan\n", L2M_VERSION);
	say(&rp, "%u groups, %u blocks of %u bytes, %u inodes\n", fs->groups, (unsigned)fs->blocks_count, fs->bs, fs->sb->s_inodes_count);

	/* pass 1: group descriptors and bitmaps */
	for (uint32_t g = 0; g < fs->groups && !cancelled; g++) {
		struct ext4_group_desc *gd = gd_get(fs, g);
		if ((fs->has_csum || fs->has_gdt_csum) && gd->bg_checksum != gd_csum(fs, g, gd))
			problem(&rp, "group %u: descriptor checksum wrong", g);
		struct l2m_buf *b;
		int r = 0;
		if (fs->sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_BIGALLOC) {
			/* bitmaps count clusters: only the checksum is checked */
			r = block_bitmap_get(fs, g, &b, false);
			if (r)
				problem(&rp, "group %u: block bitmap unreadable (%d)", g, r);
			else
				buf_put(fs, b);
			free_blocks += gd_free_blocks(fs, gd);
		} else if ((r = block_bitmap_get(fs, g, &b, false))) {
			problem(&rp, "group %u: block bitmap unreadable (%d)", g, r);
		} else {
			uint32_t n = group_nblocks(fs, g);
			uint32_t used = popcount_bits(b->data, n);
			uint32_t fr = n - used;
			if (fr != gd_free_blocks(fs, gd))
				problem(&rp, "group %u: %u free blocks in bitmap, descriptor says %u", g, fr, gd_free_blocks(fs, gd));
			free_blocks += fr;
			buf_put(fs, b);
		}
		r = inode_bitmap_get(fs, g, &b, false);
		if (r) {
			problem(&rp, "group %u: inode bitmap unreadable (%d)", g, r);
		} else {
			uint32_t used = popcount_bits(b->data, fs->ipg);
			uint32_t fr = fs->ipg - used;
			if (fr != gd_free_inodes(fs, gd))
				problem(&rp, "group %u: %u free inodes in bitmap, descriptor says %u", g, fr, gd_free_inodes(fs, gd));
			free_inodes += fr;
			used_inodes_total += used;
			buf_put(fs, b);
		}
		done++;
		if ((g & 63) == 63) {
			pthread_mutex_unlock(&fs->lock); /* let other operations run */
			pthread_mutex_lock(&fs->lock);
		}
		if (progress && (g & 63) == 0 && progress(ctx, done, total + used_inodes_total))
			cancelled = 1;
	}
	if (!cancelled) {
		if (free_blocks != sb_free_blocks(fs))
			say(&rp, "note: superblock free block count %llu, counted %llu (Linux updates it lazily)\n",
			    (unsigned long long)sb_free_blocks(fs), (unsigned long long)free_blocks);
		if (free_inodes != fs->sb->s_free_inodes_count)
			say(&rp, "note: superblock free inode count %u, counted %llu\n", fs->sb->s_free_inodes_count, (unsigned long long)free_inodes);
	}

	/* pass 2: every used inode: checksum and block map */
	uint64_t files = 0, dirs = 0, links = 0, other = 0, checked = 0;
	for (uint32_t g = 0; g < fs->groups && !cancelled; g++) {
		struct ext4_group_desc *gd = gd_get(fs, g);
		if ((fs->has_csum || fs->has_gdt_csum) && (gd->bg_flags & EXT4_BG_INODE_UNINIT)) {
			done++;
			continue;
		}
		struct l2m_buf *b;
		if (inode_bitmap_get(fs, g, &b, false)) {
			done++;
			continue;
		}
		uint32_t limit = fs->ipg;
		if (fs->has_csum || fs->has_gdt_csum)
			limit = fs->ipg - gd_itable_unused(fs, gd);
		for (uint32_t i = 0; i < limit && !cancelled; i++) {
			if (!(b->data[i >> 3] & (1u << (i & 7))))
				continue;
			uint32_t ino = g * fs->ipg + i + 1;
			if (ino < fs->first_ino && ino != EXT4_ROOT_INO)
				continue;
			struct l2m_inode in;
			int r = inode_read(fs, ino, &in);
			checked++;
			if (r) {
				problem(&rp, "inode %u: %s", ino, r == -EFSBADCRC ? "checksum wrong" : "unreadable");
				continue;
			}
			if (inode_is_reg(&in)) {
				files++;
				int64_t z = zero_holes_locked(fs, &in, NULL, NULL);
				if (z > 0) {
					if (zh.zero_hole_files < sizeof(zh.zero_hole_inos) / sizeof(*zh.zero_hole_inos))
						zh.zero_hole_inos[zh.zero_hole_files] = ino;
					zh.zero_hole_files++;
					zh.zero_hole_blocks += (uint64_t)z;
				}
			} else if (inode_is_dir(&in))
				dirs++;
			else if (inode_is_lnk(&in))
				links++;
			else
				other++;
			if ((in.i.i_flags & EXT4_EXTENTS_FL) && !inode_is_fast_symlink(fs, &in)) {
				uint64_t nb;
				r = ext_verify_tree(fs, &in, &nb);
				if (r) {
					problem(&rp, "inode %u: extent tree damaged (%d)", ino, r);
					if (rp.problems <= 20)
						ext_describe(fs, &in, say_line, &rp);
				}
			}
			if (inode_is_dir(&in)) {
				uint64_t n = inode_size(&in) / fs->bs;
				for (uint64_t lb = 0; lb < n && lb < 1u << 20; lb++) {
					struct l2m_buf *db;
					bool dx;
					r = dir_read_block(fs, &in, lb, &db);
					if (r == -ENODATA)
						continue;
					if (r) {
						problem(&rp, "directory %u block %llu unreadable", ino, (unsigned long long)lb);
						break;
					}
					r = dir_check_block(fs, &in, db->data, lb, &dx);
					buf_put(fs, db);
					if (r) {
						problem(&rp, "directory %u block %llu damaged", ino, (unsigned long long)lb);
						break;
					}
				}
			}
			if ((checked & 1023) == 0) {
				buf_put(fs, b);
				pthread_mutex_unlock(&fs->lock);
				if (progress && progress(ctx, done + checked, total + used_inodes_total))
					cancelled = 1;
				pthread_mutex_lock(&fs->lock);
				if (inode_bitmap_get(fs, g, &b, false)) {
					b = NULL;
					break;
				}
			}
		}
		if (b)
			buf_put(fs, b);
		done++;
	}
	if (!cancelled)
		cross_check(fs, &rp);
	if (!fs->rdonly)
		say(&rp, "note: the volume is writable; changes made during the scan can show up as count differences\n");
	say(&rp, "checked %llu inodes: %llu files, %llu directories, %llu symlinks, %llu other\n",
	    (unsigned long long)checked, (unsigned long long)files, (unsigned long long)dirs, (unsigned long long)links,
	    (unsigned long long)other);
	if (zh.zero_hole_files) {
		say(&rp, "note: %llu file(s) with zero holes - %llu block(s) inside them are allocated but not marked as written and "
			 "read as zeros. Legal for space reserved in advance, but in a finished file data is missing there "
			 "(ExtBridge 0.9.2 could leave such holes in big files): copy those files again.\n",
		    (unsigned long long)zh.zero_hole_files, (unsigned long long)zh.zero_hole_blocks);
		for (uint64_t i = 0; i < zh.zero_hole_files && i < sizeof(zh.zero_hole_inos) / sizeof(*zh.zero_hole_inos); i++)
			say(&rp, "  zero holes in inode %u\n", zh.zero_hole_inos[i]);
	}
	if (cancelled)
		say(&rp, "scan cancelled\n");
	else if (rp.problems == 0 && !zh.zero_hole_files)
		say(&rp, "RESULT: no problems found\n");
	else if (rp.problems == 0)
		say(&rp, "RESULT: no problems found, but %llu file(s) with zero holes\n", (unsigned long long)zh.zero_hole_files);
	else
		say(&rp, "RESULT: %d problem(s) found - repair with e2fsck on Linux\n", rp.problems);
	if (stats)
		*stats = zh;
	if (progress && !cancelled)
		progress(ctx, total + used_inodes_total, total + used_inodes_total);
	fs->verify_csums = saved;
	/* do not keep the whole inode table in memory */
	cache_invalidate_clean(fs);
	pthread_mutex_unlock(&fs->lock);
	return cancelled ? -ECANCELED : rp.problems;
}
