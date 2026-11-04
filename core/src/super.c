/*
 * ExtBridge - superblock, group descriptors, mount and unmount.
 *
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

#include <time.h>

/* ---- logging ---------------------------------------------------------- */

static l2m_log_fn g_log;

void l2m_set_log(l2m_log_fn fn) { g_log = fn; }

void l2m_log(l2m_fs *fs, int level, const char *fmt, ...)
{
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (fs && level <= L2M_LOG_WARN)
		snprintf(fs->last_error, sizeof(fs->last_error), "%s", msg);
	if (g_log)
		g_log(level, msg);
	else if (level == L2M_LOG_ERROR && getenv("L2M_DEBUG"))
		fprintf(stderr, "l2m: %s\n", msg);
	else if (getenv("L2M_DEBUG") && level <= atoi(getenv("L2M_DEBUG")))
		fprintf(stderr, "l2m[%d]: %s\n", level, msg);
}

int l2m_corrupt(l2m_fs *fs, const char *what, uint64_t where)
{
	if (fs)
		fs->errors_seen++;
	l2m_log(fs, L2M_LOG_ERROR, "corruption: %s (%llu)", what, (unsigned long long)where);
	return -EFSCORRUPTED;
}

const char *l2m_last_error(l2m_fs *fs) { return fs ? fs->last_error : ""; }

uint64_t l2m_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void l2m_now(l2m_time *t)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	t->sec = ts.tv_sec;
	t->nsec = (uint32_t)ts.tv_nsec;
}

/* ---- feature tables --------------------------------------------------- */

struct feat {
	int kind; /* 0 compat, 1 ro_compat, 2 incompat */
	uint32_t mask;
	const char *name;
	int policy; /* 0 rw ok, 1 read-only, 2 cannot mount */
};

static const struct feat features[] = {
	{0, EXT4_FEATURE_COMPAT_DIR_PREALLOC, "dir_prealloc", 0},
	{0, EXT4_FEATURE_COMPAT_IMAGIC_INODES, "imagic_inodes", 0},
	{0, EXT4_FEATURE_COMPAT_HAS_JOURNAL, "has_journal", 0},
	{0, EXT4_FEATURE_COMPAT_EXT_ATTR, "ext_attr", 0},
	{0, EXT4_FEATURE_COMPAT_RESIZE_INODE, "resize_inode", 0},
	{0, EXT4_FEATURE_COMPAT_DIR_INDEX, "dir_index", 0},
	{0, EXT4_FEATURE_COMPAT_LAZY_BG, "lazy_bg", 0},
	{0, EXT4_FEATURE_COMPAT_EXCLUDE_BITMAP, "snapshot_bitmap", 1},
	{0, EXT4_FEATURE_COMPAT_SPARSE_SUPER2, "sparse_super2", 0},
	{0, EXT4_FEATURE_COMPAT_FAST_COMMIT, "fast_commit", 0},
	{0, EXT4_FEATURE_COMPAT_STABLE_INODES, "stable_inodes", 0},
	{0, EXT4_FEATURE_COMPAT_ORPHAN_FILE, "orphan_file", 0},
	{1, EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER, "sparse_super", 0},
	{1, EXT4_FEATURE_RO_COMPAT_LARGE_FILE, "large_file", 0},
	{1, EXT4_FEATURE_RO_COMPAT_BTREE_DIR, "btree_dir", 1},
	{1, EXT4_FEATURE_RO_COMPAT_HUGE_FILE, "huge_file", 0},
	{1, EXT4_FEATURE_RO_COMPAT_GDT_CSUM, "uninit_bg", 0},
	{1, EXT4_FEATURE_RO_COMPAT_DIR_NLINK, "dir_nlink", 0},
	{1, EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE, "extra_isize", 0},
	{1, EXT4_FEATURE_RO_COMPAT_HAS_SNAPSHOT, "snapshot", 1},
	{1, EXT4_FEATURE_RO_COMPAT_QUOTA, "quota", 1},
	{1, EXT4_FEATURE_RO_COMPAT_BIGALLOC, "bigalloc", 1},
	{1, EXT4_FEATURE_RO_COMPAT_METADATA_CSUM, "metadata_csum", 0},
	{1, EXT4_FEATURE_RO_COMPAT_REPLICA, "replica", 1},
	{1, EXT4_FEATURE_RO_COMPAT_READONLY, "read-only", 1},
	{1, EXT4_FEATURE_RO_COMPAT_PROJECT, "project", 0},
	{1, EXT4_FEATURE_RO_COMPAT_SHARED_BLOCKS, "shared_blocks", 1},
	{1, EXT4_FEATURE_RO_COMPAT_VERITY, "verity", 1},
	{1, EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT, "orphan_present", 1},
	{2, EXT4_FEATURE_INCOMPAT_COMPRESSION, "compression", 2},
	{2, EXT4_FEATURE_INCOMPAT_FILETYPE, "filetype", 0},
	{2, EXT4_FEATURE_INCOMPAT_RECOVER, "needs_recovery", 0},
	{2, EXT4_FEATURE_INCOMPAT_JOURNAL_DEV, "journal_dev", 2},
	{2, EXT4_FEATURE_INCOMPAT_META_BG, "meta_bg", 1},
	{2, EXT4_FEATURE_INCOMPAT_EXTENTS, "extent", 0},
	{2, EXT4_FEATURE_INCOMPAT_64BIT, "64bit", 0},
	{2, EXT4_FEATURE_INCOMPAT_MMP, "mmp", 1},
	{2, EXT4_FEATURE_INCOMPAT_FLEX_BG, "flex_bg", 0},
	{2, EXT4_FEATURE_INCOMPAT_EA_INODE, "ea_inode", 1},
	{2, EXT4_FEATURE_INCOMPAT_DIRDATA, "dirdata", 2},
	{2, EXT4_FEATURE_INCOMPAT_CSUM_SEED, "metadata_csum_seed", 0},
	{2, EXT4_FEATURE_INCOMPAT_LARGEDIR, "large_dir", 0},
	{2, EXT4_FEATURE_INCOMPAT_INLINE_DATA, "inline_data", 1},
	{2, EXT4_FEATURE_INCOMPAT_ENCRYPT, "encrypt", 1},
	{2, EXT4_FEATURE_INCOMPAT_CASEFOLD, "casefold", 1},
};

static void append(char *dst, size_t cap, const char *s)
{
	size_t l = strlen(dst);
	if (l && l + 1 < cap)
		dst[l++] = ' ', dst[l] = 0;
	snprintf(dst + l, cap - l, "%s", s);
}

/* returns 0 rw ok, 1 ro only, 2 not mountable; fills names */
static int check_features(const struct ext4_super_block *sb, char *names, size_t ncap, char *bad, size_t bcap)
{
	uint32_t v[3] = {sb->s_feature_compat, sb->s_feature_ro_compat, sb->s_feature_incompat};
	uint32_t known[3] = {0, 0, 0};
	int worst = 0;
	if (names)
		names[0] = 0;
	if (bad)
		bad[0] = 0;
	for (size_t i = 0; i < sizeof(features) / sizeof(features[0]); i++) {
		const struct feat *f = &features[i];
		known[f->kind] |= f->mask;
		if (!(v[f->kind] & f->mask))
			continue;
		if (names)
			append(names, ncap, f->name);
		if (f->policy) {
			if (bad)
				append(bad, bcap, f->name);
			worst = L2M_MAX(worst, f->policy);
		}
	}
	if (v[1] & ~known[1]) {
		if (bad)
			append(bad, bcap, "unknown_ro_compat");
		worst = L2M_MAX(worst, 1);
	}
	if (v[2] & ~known[2]) {
		if (bad)
			append(bad, bcap, "unknown_incompat");
		worst = 2;
	}
	/* Writing needs extents and file types in directory entries. ext2/3
	 * volumes without them are offered read-only for now. */
	if (!(v[2] & EXT4_FEATURE_INCOMPAT_EXTENTS) || !(v[2] & EXT4_FEATURE_INCOMPAT_FILETYPE)) {
		if (bad)
			append(bad, bcap, (v[2] & EXT4_FEATURE_INCOMPAT_EXTENTS) ? "no_filetype" : "no_extents(ext2/3)");
		worst = L2M_MAX(worst, 1);
	}
	if (sb->s_rev_level == 0) {
		if (bad)
			append(bad, bcap, "revision0");
		worst = L2M_MAX(worst, 1);
	}
	return worst;
}

/* ---- superblock -------------------------------------------------------- */

static uint32_t sb_csum(const struct ext4_super_block *sb)
{
	return l2m_crc32c(~0u, sb, offsetof(struct ext4_super_block, s_checksum));
}

static int is_power_of(uint32_t n, uint32_t base)
{
	if (n == 0)
		return 0;
	while (n % base == 0)
		n /= base;
	return n == 1;
}

int group_has_super(l2m_fs *fs, uint32_t g)
{
	if (g == 0)
		return 1;
	if (fs->sb->s_feature_compat & EXT4_FEATURE_COMPAT_SPARSE_SUPER2)
		return g == fs->sb->s_backup_bgs[0] || g == fs->sb->s_backup_bgs[1];
	if (!(fs->sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER))
		return 1;
	if (g <= 1)
		return 1;
	if (!(g & 1))
		return 0;
	return is_power_of(g, 3) || is_power_of(g, 5) || is_power_of(g, 7);
}

uint64_t group_first_block(l2m_fs *fs, uint32_t g)
{
	return (uint64_t)g * fs->bpg + fs->first_data_block;
}

uint32_t group_nblocks(l2m_fs *fs, uint32_t g)
{
	if (g + 1 < fs->groups)
		return fs->bpg;
	return (uint32_t)(fs->blocks_count - group_first_block(fs, g));
}

/* block of the i-th group descriptor block */
static uint64_t gdt_block_loc(l2m_fs *fs, uint32_t i)
{
	uint64_t sb_blk = fs->first_data_block;
	if (!fs->has_meta_bg || i < fs->sb->s_first_meta_bg)
		return sb_blk + 1 + i;
	uint32_t g = i * fs->desc_per_block;
	return group_first_block(fs, g) + (group_has_super(fs, g) ? 1 : 0);
}

static int sb_parse(l2m_fs *fs, char *err, size_t errlen)
{
	struct ext4_super_block *sb = fs->sb;
	if (sb->s_magic != EXT4_SUPER_MAGIC) {
		snprintf(err, errlen, "no ext2/3/4 file system (bad magic)");
		return -EINVAL;
	}
	if (sb->s_log_block_size > 6) {
		snprintf(err, errlen, "invalid block size");
		return -EFSCORRUPTED;
	}
	fs->log_bs = 10 + sb->s_log_block_size;
	fs->bs = 1024u << sb->s_log_block_size;
	fs->is64 = sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT;
	fs->blocks_count = sb->s_blocks_count_lo | (fs->is64 ? (uint64_t)sb->s_blocks_count_hi << 32 : 0);
	fs->first_data_block = sb->s_first_data_block;
	fs->bpg = sb->s_blocks_per_group;
	fs->ipg = sb->s_inodes_per_group;
	fs->inode_size = sb->s_rev_level == 0 ? EXT4_GOOD_OLD_INODE_SIZE : sb->s_inode_size;
	fs->first_ino = sb->s_rev_level == 0 ? EXT4_GOOD_OLD_FIRST_INO : sb->s_first_ino;
	fs->desc_size = fs->is64 ? sb->s_desc_size : EXT4_DESC_SIZE_32;
	if (fs->bpg == 0 || fs->ipg == 0 || sb->s_clusters_per_group > fs->bs * 8 || fs->ipg > fs->bs * 8 ||
	    fs->inode_size < EXT4_GOOD_OLD_INODE_SIZE || fs->inode_size > fs->bs ||
	    fs->inode_size > L2M_MAX_INODE_SIZE || (fs->inode_size & (fs->inode_size - 1)) ||
	    fs->desc_size < EXT4_DESC_SIZE_32 || fs->desc_size > 1024 || (fs->desc_size & (fs->desc_size - 1)) ||
	    fs->first_data_block > 1 || fs->blocks_count <= fs->first_data_block ||
	    (fs->bs == 1024 && fs->first_data_block != 1 && fs->sb->s_log_cluster_size == 0) ||
	    fs->first_ino < EXT4_GOOD_OLD_FIRST_INO - 1 || fs->ipg % (fs->bs / fs->inode_size) != 0) {
		snprintf(err, errlen, "superblock contains invalid geometry");
		return -EFSCORRUPTED;
	}
	if ((sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_BIGALLOC) && sb->s_log_cluster_size != sb->s_log_block_size) {
		/* bitmaps are per cluster; we only read such volumes and never
		 * touch the bitmaps, so this is fine for ro */
	}
	fs->groups = (uint32_t)L2M_DIV_ROUND_UP(fs->blocks_count - fs->first_data_block, fs->bpg);
	if ((uint64_t)fs->groups * fs->ipg != sb->s_inodes_count) {
		snprintf(err, errlen, "inode count does not match group count");
		return -EFSCORRUPTED;
	}
	fs->desc_per_block = fs->bs / fs->desc_size;
	fs->gdt_blocks = L2M_DIV_ROUND_UP(fs->groups, fs->desc_per_block);
	fs->inode_table_blocks = fs->ipg * fs->inode_size / fs->bs;
	if (fs->dev.size / fs->bs < fs->blocks_count) {
		snprintf(err, errlen, "device is smaller than the file system (%llu < %llu blocks)",
			 (unsigned long long)(fs->dev.size / fs->bs), (unsigned long long)fs->blocks_count);
		return -EINVAL;
	}

	fs->has_csum = sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM;
	fs->has_gdt_csum = !fs->has_csum && (sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_GDT_CSUM);
	fs->has_extents = sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS;
	fs->has_flex = sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_FLEX_BG;
	fs->has_dir_index = sb->s_feature_compat & EXT4_FEATURE_COMPAT_DIR_INDEX;
	fs->has_filetype = sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE;
	fs->has_huge_file = sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_HUGE_FILE;
	fs->has_dir_nlink = sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_DIR_NLINK;
	fs->has_extra_isize = sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE;
	fs->has_largedir = sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_LARGEDIR;
	fs->has_meta_bg = sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_META_BG;
	fs->has_journal = sb->s_feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL;
	fs->has_xattr = sb->s_feature_compat & EXT4_FEATURE_COMPAT_EXT_ATTR;
	fs->has_inline_data = sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_INLINE_DATA;
	fs->has_sparse_super2 = sb->s_feature_compat & EXT4_FEATURE_COMPAT_SPARSE_SUPER2;

	if (fs->has_csum) {
		if (sb->s_checksum_type != EXT4_CRC32C_CHKSUM) {
			snprintf(err, errlen, "unknown metadata checksum type %u", sb->s_checksum_type);
			return -EINVAL;
		}
		if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED)
			fs->csum_seed = sb->s_checksum_seed;
		else
			fs->csum_seed = l2m_crc32c(~0u, sb->s_uuid, 16);
	}
	memcpy(fs->hash_seed, sb->s_hash_seed, sizeof(fs->hash_seed));
	fs->def_hash = sb->s_def_hash_version;
	if (sb->s_flags & EXT2_FLAGS_UNSIGNED_HASH)
		fs->hash_unsigned = 3;
	else if (sb->s_flags & EXT2_FLAGS_SIGNED_HASH)
		fs->hash_unsigned = 0;
	else
		fs->hash_unsigned = 0; /* Linux default on x86/arm: signed */
	fs->want_extra_isize = 0;
	if (fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE) {
		fs->want_extra_isize = sb->s_want_extra_isize;
		if (fs->want_extra_isize < sizeof(struct ext4_inode) - EXT4_GOOD_OLD_INODE_SIZE)
			fs->want_extra_isize = sizeof(struct ext4_inode) - EXT4_GOOD_OLD_INODE_SIZE;
		if (fs->want_extra_isize > fs->inode_size - EXT4_GOOD_OLD_INODE_SIZE)
			fs->want_extra_isize = (uint16_t)(fs->inode_size - EXT4_GOOD_OLD_INODE_SIZE);
	}
	return 0;
}

int sb_dirty(l2m_fs *fs)
{
	return buf_dirty(fs, fs->sbbuf);
}

/* called right before the superblock buffer is written (journal commit) */
void sb_finalize(l2m_fs *fs);
void sb_finalize(l2m_fs *fs)
{
	if (fs->has_csum)
		fs->sb->s_checksum = sb_csum(fs->sb);
}

uint64_t sb_free_blocks(l2m_fs *fs)
{
	return fs->sb->s_free_blocks_count_lo | (fs->is64 ? (uint64_t)fs->sb->s_free_blocks_count_hi << 32 : 0);
}

void sb_add_free_blocks(l2m_fs *fs, int64_t delta)
{
	uint64_t v = sb_free_blocks(fs) + (uint64_t)delta;
	fs->sb->s_free_blocks_count_lo = (uint32_t)v;
	if (fs->is64)
		fs->sb->s_free_blocks_count_hi = (uint32_t)(v >> 32);
	sb_dirty(fs);
}

void sb_add_free_inodes(l2m_fs *fs, int64_t delta)
{
	fs->sb->s_free_inodes_count = (uint32_t)((int64_t)fs->sb->s_free_inodes_count + delta);
	sb_dirty(fs);
}

/* ---- group descriptors -------------------------------------------------- */

struct ext4_group_desc *gd_get(l2m_fs *fs, uint32_t group)
{
	if (group >= fs->groups)
		return NULL;
	uint32_t i = group / fs->desc_per_block, o = (group % fs->desc_per_block) * fs->desc_size;
	return (struct ext4_group_desc *)(fs->gdt[i]->data + o);
}

#define HI(fs, field) ((fs)->desc_size >= 64 ? (field) : 0)

uint64_t gd_block_bitmap(l2m_fs *fs, const struct ext4_group_desc *gd) { return gd->bg_block_bitmap_lo | ((uint64_t)HI(fs, gd->bg_block_bitmap_hi) << 32); }
uint64_t gd_inode_bitmap(l2m_fs *fs, const struct ext4_group_desc *gd) { return gd->bg_inode_bitmap_lo | ((uint64_t)HI(fs, gd->bg_inode_bitmap_hi) << 32); }
uint64_t gd_inode_table(l2m_fs *fs, const struct ext4_group_desc *gd) { return gd->bg_inode_table_lo | ((uint64_t)HI(fs, gd->bg_inode_table_hi) << 32); }
uint32_t gd_free_blocks(l2m_fs *fs, const struct ext4_group_desc *gd) { return gd->bg_free_blocks_count_lo | ((uint32_t)HI(fs, gd->bg_free_blocks_count_hi) << 16); }
uint32_t gd_free_inodes(l2m_fs *fs, const struct ext4_group_desc *gd) { return gd->bg_free_inodes_count_lo | ((uint32_t)HI(fs, gd->bg_free_inodes_count_hi) << 16); }
uint32_t gd_used_dirs(l2m_fs *fs, const struct ext4_group_desc *gd) { return gd->bg_used_dirs_count_lo | ((uint32_t)HI(fs, gd->bg_used_dirs_count_hi) << 16); }
uint32_t gd_itable_unused(l2m_fs *fs, const struct ext4_group_desc *gd) { return gd->bg_itable_unused_lo | ((uint32_t)HI(fs, gd->bg_itable_unused_hi) << 16); }

void gd_set_free_blocks(l2m_fs *fs, struct ext4_group_desc *gd, uint32_t v)
{
	gd->bg_free_blocks_count_lo = (uint16_t)v;
	if (fs->desc_size >= 64)
		gd->bg_free_blocks_count_hi = (uint16_t)(v >> 16);
}
void gd_set_free_inodes(l2m_fs *fs, struct ext4_group_desc *gd, uint32_t v)
{
	gd->bg_free_inodes_count_lo = (uint16_t)v;
	if (fs->desc_size >= 64)
		gd->bg_free_inodes_count_hi = (uint16_t)(v >> 16);
}
void gd_set_used_dirs(l2m_fs *fs, struct ext4_group_desc *gd, uint32_t v)
{
	gd->bg_used_dirs_count_lo = (uint16_t)v;
	if (fs->desc_size >= 64)
		gd->bg_used_dirs_count_hi = (uint16_t)(v >> 16);
}
void gd_set_itable_unused(l2m_fs *fs, struct ext4_group_desc *gd, uint32_t v)
{
	gd->bg_itable_unused_lo = (uint16_t)v;
	if (fs->desc_size >= 64)
		gd->bg_itable_unused_hi = (uint16_t)(v >> 16);
}

uint16_t gd_csum(l2m_fs *fs, uint32_t group, const struct ext4_group_desc *gd)
{
	uint32_t le_group = group;
	size_t off = offsetof(struct ext4_group_desc, bg_checksum);
	uint16_t dummy = 0;
	if (fs->has_csum) {
		uint32_t c = l2m_crc32c(fs->csum_seed, &le_group, 4);
		c = l2m_crc32c(c, gd, off);
		c = l2m_crc32c(c, &dummy, 2);
		if (off + 2 < fs->desc_size)
			c = l2m_crc32c(c, (const uint8_t *)gd + off + 2, fs->desc_size - off - 2);
		return (uint16_t)(c & 0xFFFF);
	}
	if (fs->has_gdt_csum) {
		uint16_t c = l2m_crc16(0xFFFF, fs->sb->s_uuid, 16);
		c = l2m_crc16(c, &le_group, 4);
		c = l2m_crc16(c, gd, off);
		if (fs->is64 && off + 2 < fs->desc_size)
			c = l2m_crc16(c, (const uint8_t *)gd + off + 2, fs->desc_size - off - 2);
		return c;
	}
	return 0;
}

int gd_dirty(l2m_fs *fs, uint32_t group)
{
	struct ext4_group_desc *gd = gd_get(fs, group);
	if (fs->has_csum || fs->has_gdt_csum)
		gd->bg_checksum = gd_csum(fs, group, gd);
	return buf_dirty(fs, fs->gdt[group / fs->desc_per_block]);
}

uint32_t bitmap_csum(l2m_fs *fs, const uint8_t *bitmap, uint32_t bytes)
{
	return l2m_crc32c(fs->csum_seed, bitmap, bytes);
}

int block_bitmap_update_csum(l2m_fs *fs, uint32_t group, struct l2m_buf *b)
{
	struct ext4_group_desc *gd = gd_get(fs, group);
	if (fs->has_csum) {
		uint32_t c = bitmap_csum(fs, b->data, fs->sb->s_clusters_per_group / 8);
		gd->bg_block_bitmap_csum_lo = (uint16_t)c;
		if (fs->desc_size >= 64)
			gd->bg_block_bitmap_csum_hi = (uint16_t)(c >> 16);
	}
	return gd_dirty(fs, group);
}

int inode_bitmap_update_csum(l2m_fs *fs, uint32_t group, struct l2m_buf *b)
{
	struct ext4_group_desc *gd = gd_get(fs, group);
	if (fs->has_csum) {
		uint32_t c = bitmap_csum(fs, b->data, fs->ipg / 8);
		gd->bg_inode_bitmap_csum_lo = (uint16_t)c;
		if (fs->desc_size >= 64)
			gd->bg_inode_bitmap_csum_hi = (uint16_t)(c >> 16);
	}
	return gd_dirty(fs, group);
}

static void set_bit_range(uint8_t *bm, uint64_t from, uint64_t to /* exclusive */)
{
	for (uint64_t i = from; i < to; i++)
		bm[i >> 3] |= (uint8_t)(1u << (i & 7));
}

/* Number of blocks used by superblock + descriptors (+ reserved GDT) at
 * the start of a group that carries a backup. */
uint32_t group_super_overhead(l2m_fs *fs, uint32_t g)
{
	if (!group_has_super(fs, g))
		return 0;
	uint32_t n = 1;
	if (fs->has_meta_bg) {
		/* meta_bg: descriptor blocks live in the first, second and last
		 * group of each meta group */
		uint32_t mg = g / fs->desc_per_block, first_meta = fs->sb->s_first_meta_bg;
		if (mg < first_meta)
			n += fs->gdt_blocks + fs->sb->s_reserved_gdt_blocks;
		return n;
	}
	n += fs->gdt_blocks + fs->sb->s_reserved_gdt_blocks;
	return n;
}

/* Build the block bitmap of a BLOCK_UNINIT group (like Linux'
 * ext4_init_block_bitmap). */
static void init_block_bitmap(l2m_fs *fs, uint32_t g, uint8_t *bm)
{
	uint64_t first = group_first_block(fs, g);
	uint32_t n = group_nblocks(fs, g);
	memset(bm, 0, fs->bs);
	set_bit_range(bm, 0, group_super_overhead(fs, g));
	if (fs->has_meta_bg) {
		uint32_t mg = g / fs->desc_per_block;
		if (mg >= fs->sb->s_first_meta_bg) {
			uint32_t rel = g % fs->desc_per_block;
			if (rel == 0 || rel == 1 || rel == fs->desc_per_block - 1)
				set_bit_range(bm, group_has_super(fs, g), group_has_super(fs, g) + 1u);
		}
	}
	/* metadata of all groups that lives inside this group (flex_bg) */
	for (uint32_t h = 0; h < fs->groups; h++) {
		struct ext4_group_desc *gd = gd_get(fs, h);
		uint64_t locs[3] = {gd_block_bitmap(fs, gd), gd_inode_bitmap(fs, gd), gd_inode_table(fs, gd)};
		uint32_t lens[3] = {1, 1, fs->inode_table_blocks};
		for (int k = 0; k < 3; k++) {
			if (locs[k] + lens[k] <= first || locs[k] >= first + n)
				continue;
			uint64_t a = L2M_MAX(locs[k], first), b = L2M_MIN(locs[k] + lens[k], first + n);
			set_bit_range(bm, a - first, b - first);
		}
		if (!fs->has_flex && h > g)
			break;
	}
	/* padding past the end of the last group */
	set_bit_range(bm, n, (uint64_t)fs->bs * 8);
}

int block_bitmap_get(l2m_fs *fs, uint32_t g, struct l2m_buf **out, bool for_write)
{
	struct ext4_group_desc *gd = gd_get(fs, g);
	uint64_t blk = gd_block_bitmap(fs, gd);
	int r;
	if (blk < fs->first_data_block || blk >= fs->blocks_count)
		return l2m_corrupt(fs, "block bitmap location invalid in group", g);
	if ((fs->has_csum || fs->has_gdt_csum) && (gd->bg_flags & EXT4_BG_BLOCK_UNINIT)) {
		r = buf_get_new(fs, blk, out);
		if (r)
			return r;
		init_block_bitmap(fs, g, (*out)->data);
		if (for_write) {
			gd->bg_flags &= (uint16_t)~EXT4_BG_BLOCK_UNINIT;
			r = buf_dirty(fs, *out);
			if (!r)
				r = block_bitmap_update_csum(fs, g, *out);
			if (r) {
				buf_put(fs, *out);
				return r;
			}
		}
		return 0;
	}
	r = buf_get(fs, blk, out);
	if (r)
		return r;
	if (fs->has_csum && fs->verify_csums && !(*out)->dirty) {
		uint32_t c = bitmap_csum(fs, (*out)->data, fs->sb->s_clusters_per_group / 8);
		uint32_t want = gd->bg_block_bitmap_csum_lo | (fs->desc_size >= 64 ? (uint32_t)gd->bg_block_bitmap_csum_hi << 16 : 0);
		if (fs->desc_size < 64)
			c &= 0xFFFF;
		if (c != want) {
			buf_put(fs, *out);
			*out = NULL;
			l2m_log(fs, L2M_LOG_ERROR, "block bitmap checksum mismatch in group %u", g);
			return -EFSBADCRC;
		}
	}
	return 0;
}

int inode_bitmap_get(l2m_fs *fs, uint32_t g, struct l2m_buf **out, bool for_write)
{
	struct ext4_group_desc *gd = gd_get(fs, g);
	uint64_t blk = gd_inode_bitmap(fs, gd);
	int r;
	if (blk < fs->first_data_block || blk >= fs->blocks_count)
		return l2m_corrupt(fs, "inode bitmap location invalid in group", g);
	if ((fs->has_csum || fs->has_gdt_csum) && (gd->bg_flags & EXT4_BG_INODE_UNINIT)) {
		r = buf_get_new(fs, blk, out);
		if (r)
			return r;
		/* bits past inodes_per_group are set (padding) */
		set_bit_range((*out)->data, fs->ipg, (uint64_t)fs->bs * 8);
		if (for_write) {
			gd->bg_flags &= (uint16_t)~EXT4_BG_INODE_UNINIT;
			r = buf_dirty(fs, *out);
			if (!r)
				r = inode_bitmap_update_csum(fs, g, *out);
			if (r) {
				buf_put(fs, *out);
				return r;
			}
		}
		return 0;
	}
	r = buf_get(fs, blk, out);
	if (r)
		return r;
	if (fs->has_csum && fs->verify_csums && !(*out)->dirty) {
		uint32_t c = bitmap_csum(fs, (*out)->data, fs->ipg / 8);
		uint32_t want = gd->bg_inode_bitmap_csum_lo | (fs->desc_size >= 64 ? (uint32_t)gd->bg_inode_bitmap_csum_hi << 16 : 0);
		if (fs->desc_size < 64)
			c &= 0xFFFF;
		if (c != want) {
			buf_put(fs, *out);
			*out = NULL;
			l2m_log(fs, L2M_LOG_ERROR, "inode bitmap checksum mismatch in group %u", g);
			return -EFSBADCRC;
		}
	}
	return 0;
}

/* ---- mount ------------------------------------------------------------- */

static int load_gdt(l2m_fs *fs, char *err, size_t errlen)
{
	fs->ngdt = fs->gdt_blocks;
	fs->gdt = calloc(fs->ngdt, sizeof(*fs->gdt));
	if (!fs->gdt)
		return -ENOMEM;
	for (uint32_t i = 0; i < fs->ngdt; i++) {
		int r = buf_get(fs, gdt_block_loc(fs, i), &fs->gdt[i]);
		if (r) {
			snprintf(err, errlen, "cannot read group descriptors (%d)", r);
			return r;
		}
		fs->gdt[i]->pinned = 1;
	}
	return 0;
}

static int check_gdt(l2m_fs *fs, char *err, size_t errlen)
{
	uint32_t bad = 0;
	for (uint32_t g = 0; g < fs->groups; g++) {
		struct ext4_group_desc *gd = gd_get(fs, g);
		uint64_t first = fs->first_data_block, last = fs->blocks_count - 1;
		uint64_t bb = gd_block_bitmap(fs, gd), ib = gd_inode_bitmap(fs, gd), it = gd_inode_table(fs, gd);
		if (bb < first || bb > last || ib < first || ib > last || it < first || it + fs->inode_table_blocks - 1 > last) {
			snprintf(err, errlen, "group %u descriptor points outside the file system", g);
			return -EFSCORRUPTED;
		}
		if ((fs->has_csum || fs->has_gdt_csum) && gd->bg_checksum != gd_csum(fs, g, gd)) {
			bad++;
			l2m_log(fs, L2M_LOG_ERROR, "group descriptor %u checksum mismatch", g);
		}
	}
	if (bad) {
		snprintf(err, errlen, "%u group descriptor checksum(s) invalid", bad);
		return -EFSBADCRC;
	}
	return 0;
}

int fs_check_writable(l2m_fs *fs)
{
	return fs->rdonly ? -EROFS : 0;
}

int l2m_probe(const l2m_dev *dev, char *label, char *uuid, uint64_t *size_bytes)
{
	struct ext4_super_block sb;
	if (dev->size < 2048 + sizeof(sb))
		return 0;
	int r = dev->read(dev->ctx, EXT4_SUPERBLOCK_OFFSET, &sb, sizeof(sb));
	if (r < 0)
		return r;
	if (sb.s_magic != EXT4_SUPER_MAGIC || sb.s_log_block_size > 6)
		return 0;
	if ((sb.s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) && sb.s_checksum != sb_csum(&sb))
		return 0;
	if (label) {
		memcpy(label, sb.s_volume_name, 16);
		label[16] = 0;
	}
	if (uuid) {
		const uint8_t *u = sb.s_uuid;
		snprintf(uuid, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			 u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
	}
	if (size_bytes) {
		uint64_t bc = sb.s_blocks_count_lo;
		if (sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
			bc |= (uint64_t)sb.s_blocks_count_hi << 32;
		*size_bytes = bc << (10 + sb.s_log_block_size);
	}
	return 1;
}

static void fs_free(l2m_fs *fs)
{
	if (!fs)
		return;
	journal_free(fs);
	free(fs->gdt);
	cache_destroy(fs);
	free(fs->txn.freed);
	free(fs->zero_block);
	free(fs->koio);
	pthread_mutex_destroy(&fs->lock);
	free(fs);
}

static int mark_mounted(l2m_fs *fs)
{
	struct ext4_super_block *sb = fs->sb;
	l2m_time now;
	l2m_now(&now);
	sb->s_mnt_count++;
	sb->s_mtime = (uint32_t)now.sec;
	sb->s_mtime_hi = (uint8_t)(now.sec >> 32);
	if (fs->has_journal)
		sb->s_feature_incompat |= EXT4_FEATURE_INCOMPAT_RECOVER;
	else
		sb->s_state &= (uint16_t)~EXT4_VALID_FS;
	int r = sb_dirty(fs);
	if (r)
		return r;
	return txn_commit(fs);
}

int l2m_mount(const l2m_dev *dev, unsigned flags, l2m_fs **out, char *err, size_t errlen)
{
	char dummy[8];
	if (!err) {
		err = dummy;
		errlen = sizeof(dummy);
	}
	err[0] = 0;
	*out = NULL;
	l2m_fs *fs = calloc(1, sizeof(*fs));
	if (!fs)
		return -ENOMEM;
	/* recursive: callbacks (readdir, verify progress) may call back into
	 * read-only API functions such as l2m_getattr() */
	pthread_mutexattr_t ma;
	pthread_mutexattr_init(&ma);
	pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&fs->lock, &ma);
	pthread_mutexattr_destroy(&ma);
	fs->dev = *dev;
	fs->flags = flags;
	fs->rdonly = 1; /* until everything is verified */
	fs->verify_csums = getenv("L2M_IGNORE_CSUM") ? 0 : 1;
	fs->commit_interval_ms = 5000;

	/* bootstrap: read the superblock with a temporary 1 KiB geometry */
	struct ext4_super_block tmp;
	int r = dev->read(dev->ctx, EXT4_SUPERBLOCK_OFFSET, &tmp, sizeof(tmp));
	if (r < 0) {
		snprintf(err, errlen, "cannot read superblock (%d)", r);
		fs_free(fs);
		return r;
	}
	fs->sb = &tmp;
	r = sb_parse(fs, err, errlen);
	if (r) {
		fs_free(fs);
		return r;
	}
	if (fs->has_csum && tmp.s_checksum != sb_csum(&tmp)) {
		snprintf(err, errlen, "superblock checksum is wrong");
		fs_free(fs);
		return -EFSBADCRC;
	}
	fs->sb_orig = tmp;
	char names[512], bad[256];
	int pol = check_features(&tmp, names, sizeof(names), bad, sizeof(bad));
	snprintf(fs->unsupported, sizeof(fs->unsupported), "%s", bad);
	if (pol == 2) {
		snprintf(err, errlen, "unsupported ext4 features: %s", bad);
		fs_free(fs);
		return -EOPNOTSUPP;
	}

	r = cache_init(fs);
	if (r) {
		fs_free(fs);
		return r;
	}
	const char *lost = getenv("L2M_KOIO_LOST_MS"); /* tests */
	fs->koio_lost_ms = lost ? strtoull(lost, NULL, 10) : 60000;
	fs->zero_block = calloc(1, fs->bs);
	if (!fs->zero_block) {
		fs_free(fs);
		return -ENOMEM;
	}
	uint64_t sbblk = fs->bs == 1024 ? 1 : 0;
	r = buf_get(fs, sbblk, &fs->sbbuf);
	if (r) {
		snprintf(err, errlen, "cannot read superblock block (%d)", r);
		fs_free(fs);
		return r;
	}
	fs->sbbuf->pinned = 1;
	fs->sb = (struct ext4_super_block *)(fs->sbbuf->data + (fs->bs == 1024 ? 0 : 1024));

	r = load_gdt(fs, err, errlen);
	if (r) {
		fs_free(fs);
		return r;
	}

	int want_rw = !(flags & L2M_MOUNT_RDONLY);
	if (pol == 1 && want_rw) {
		want_rw = 0;
		fs->ro_reason = L2M_RO_FEATURE;
		l2m_log(fs, L2M_LOG_WARN, "mounting read-only because of: %s", bad);
	}
	if (!(flags & L2M_MOUNT_RDONLY) && fs->sb->s_state & EXT4_ERROR_FS && want_rw) {
		want_rw = 0;
		fs->ro_reason = L2M_RO_ERRORS;
		l2m_log(fs, L2M_LOG_WARN, "file system has errors: mounting read-only, run e2fsck on Linux");
	}
	if (!want_rw && !fs->ro_reason)
		fs->ro_reason = L2M_RO_REQUESTED;

	/* journal */
	if (fs->has_journal) {
		r = journal_load(fs);
		if (r) {
			if (want_rw || (fs->sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER)) {
				l2m_log(fs, L2M_LOG_WARN, "journal unusable (%d): read-only", r);
			}
			want_rw = 0;
			fs->ro_reason = L2M_RO_JOURNAL;
			fs->journal.present = 0;
		}
	}
	if (fs->journal.present && (fs->sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER)) {
		int to_mem = !want_rw || (flags & L2M_MOUNT_FORCE_RO_REPLAY);
		if (!to_mem)
			fs->rdonly = 0; /* replay writes */
		r = journal_recover(fs, to_mem);
		if (!to_mem)
			fs->rdonly = 1;
		if (r < 0 || fs->journal.recovery_errors) {
			if (to_mem) {
				l2m_log(fs, L2M_LOG_WARN, "journal replay (in memory) had problems: %d", r);
			} else {
				snprintf(err, errlen, "journal recovery failed (%d); please run e2fsck on Linux", r);
				fs_free(fs);
				return r < 0 ? r : -EIO;
			}
			want_rw = 0;
			fs->ro_reason = L2M_RO_JOURNAL;
		}
		/* replay may have rewritten superblock and descriptors */
		char e2[128];
		r = sb_parse(fs, e2, sizeof(e2));
		if (r) {
			snprintf(err, errlen, "after journal replay: %s", e2);
			fs_free(fs);
			return r;
		}
		if (fs->groups / fs->desc_per_block + 1 < fs->ngdt || L2M_DIV_ROUND_UP(fs->groups, fs->desc_per_block) != fs->ngdt) {
			snprintf(err, errlen, "group count changed during journal replay (resize?)");
			fs_free(fs);
			return -EFSCORRUPTED;
		}
	}
	if (!fs->has_journal && want_rw && !(fs->sb->s_state & EXT4_VALID_FS)) {
		want_rw = 0;
		fs->ro_reason = L2M_RO_NOT_CLEAN;
		l2m_log(fs, L2M_LOG_WARN, "file system was not cleanly unmounted: read-only");
	}

	r = check_gdt(fs, err, errlen);
	if (r) {
		if (want_rw || r != -EFSBADCRC) {
			if (r != -EFSBADCRC) {
				fs_free(fs);
				return r;
			}
		}
		if (want_rw) {
			want_rw = 0;
			fs->ro_reason = L2M_RO_ERRORS;
		}
		l2m_log(fs, L2M_LOG_WARN, "%s - continuing read-only", err);
		err[0] = 0;
	}

	if (want_rw) {
		fs->rdonly = 0;
		fs->ro_reason = L2M_RO_NONE;
		fs->txn_limit = txn_max_blocks(fs);
		r = orphan_cleanup(fs);
		if (!r)
			r = mark_mounted(fs);
		if (r) {
			snprintf(err, errlen, "cannot prepare volume for writing (%d)", r);
			fs->rdonly = 1;
			fs_free(fs);
			return r;
		}
	}
	*out = fs;
	return 0;
}

int fs_unmount_locked(l2m_fs *fs);
int fs_unmount_locked(l2m_fs *fs)
{
	int r = 0;
	if (!fs->rdonly) {
		r = txn_commit(fs);
		if (!r) {
			struct ext4_super_block *sb = fs->sb;
			l2m_time now;
			l2m_now(&now);
			sb->s_wtime = (uint32_t)now.sec;
			sb->s_wtime_hi = (uint8_t)(now.sec >> 32);
			sb->s_kbytes_written += fs->bytes_written / 1024;
			fs->bytes_written = 0;
			if (fs->has_journal)
				sb->s_feature_incompat &= ~(uint32_t)EXT4_FEATURE_INCOMPAT_RECOVER;
			else
				sb->s_state |= EXT4_VALID_FS;
			r = sb_dirty(fs);
			if (!r)
				r = txn_commit(fs);
		}
		if (!r)
			r = dev_flush(fs);
		if (r)
			l2m_log(fs, L2M_LOG_ERROR, "unmount: could not write final state (%d)", r);
	}
	return r;
}

int l2m_umount(l2m_fs *fs)
{
	if (!fs)
		return 0;
	pthread_mutex_lock(&fs->lock);
	/* whatever the kernel was handed for writing is on the disk by now */
	int r = fs->rdonly ? 0 : koio_release_all_locked(fs);
	if (r)
		l2m_log(fs, L2M_LOG_ERROR, "unmount: could not mark written ranges (%d)", r);
	r = fs_unmount_locked(fs);
	pthread_mutex_unlock(&fs->lock);
	fs_free(fs);
	return r;
}

int l2m_is_readonly(l2m_fs *fs) { return fs->rdonly; }

/* ---- information ------------------------------------------------------- */

static void fmt_uuid(const uint8_t *u, char *out)
{
	snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

static int64_t t40(uint32_t lo, uint8_t hi) { return (int64_t)lo | ((int64_t)hi << 32); }

int l2m_get_info(l2m_fs *fs, l2m_info *info)
{
	pthread_mutex_lock(&fs->lock);
	struct ext4_super_block *sb = fs->sb;
	memset(info, 0, sizeof(*info));
	memcpy(info->label, sb->s_volume_name, 16);
	fmt_uuid(sb->s_uuid, info->uuid);
	memcpy(info->last_mounted, sb->s_last_mounted, 64);
	check_features(sb, info->features, sizeof(info->features), NULL, 0);
	snprintf(info->unsupported, sizeof(info->unsupported), "%s", fs->unsupported);
	info->block_size = fs->bs;
	info->blocks_count = fs->blocks_count;
	info->free_blocks = sb_free_blocks(fs);
	info->reserved_blocks = sb->s_r_blocks_count_lo | (fs->is64 ? (uint64_t)sb->s_r_blocks_count_hi << 32 : 0);
	info->inodes_count = sb->s_inodes_count;
	info->free_inodes = sb->s_free_inodes_count;
	info->inode_size = fs->inode_size;
	info->groups = fs->groups;
	info->blocks_per_group = fs->bpg;
	info->inodes_per_group = fs->ipg;
	info->state = fs->sb_orig.s_state;
	info->errors_behaviour = sb->s_errors;
	info->mount_count = sb->s_mnt_count;
	info->max_mount_count = sb->s_max_mnt_count;
	info->error_count = sb->s_error_count;
	info->mkfs_time = t40(sb->s_mkfs_time, sb->s_mkfs_time_hi);
	info->mount_time = t40(sb->s_mtime, sb->s_mtime_hi);
	info->write_time = t40(sb->s_wtime, sb->s_wtime_hi);
	info->lastcheck_time = t40(sb->s_lastcheck, sb->s_lastcheck_hi);
	info->first_error_time = t40(sb->s_first_error_time, sb->s_first_error_time_hi);
	info->last_error_time = t40(sb->s_last_error_time, sb->s_last_error_time_hi);
	info->kbytes_written = sb->s_kbytes_written;
	info->creator_os = sb->s_creator_os;
	info->rev_level = sb->s_rev_level;
	info->has_journal = fs->has_journal;
	info->journal_blocks = fs->journal.maxlen;
	info->journal_features = fs->journal.incompat;
	info->journal_needed_recovery = fs->journal.needed_recovery;
	info->journal_replayed_transactions = fs->journal.replayed_trans;
	info->journal_replayed_blocks = fs->journal.replayed_blocks;
	info->metadata_csum = fs->has_csum;
	info->readonly = fs->rdonly;
	info->ro_reason = fs->ro_reason;
	static const char *hn[] = {"legacy", "half_md4", "tea", "legacy_u", "half_md4_u", "tea_u", "siphash"};
	snprintf(info->hash_algo, sizeof(info->hash_algo), "%s", fs->def_hash < 7 ? hn[fs->def_hash] : "?");
	pthread_mutex_unlock(&fs->lock);
	return 0;
}

int l2m_set_label(l2m_fs *fs, const char *label)
{
	size_t n = strlen(label);
	if (n > sizeof(fs->sb->s_volume_name))
		return -ENAMETOOLONG;
	pthread_mutex_lock(&fs->lock);
	int r = fs_check_writable(fs);
	if (!r) {
		memset(fs->sb->s_volume_name, 0, sizeof(fs->sb->s_volume_name));
		memcpy(fs->sb->s_volume_name, label, n);
		r = sb_dirty(fs);
		if (!r)
			r = txn_commit(fs);
	}
	pthread_mutex_unlock(&fs->lock);
	return r;
}

int l2m_statfs_get(l2m_fs *fs, l2m_statfs *st)
{
	pthread_mutex_lock(&fs->lock);
	uint64_t r_blocks = fs->sb->s_r_blocks_count_lo | (fs->is64 ? (uint64_t)fs->sb->s_r_blocks_count_hi << 32 : 0);
	uint64_t overhead = fs->sb->s_overhead_clusters;
	st->block_size = fs->bs;
	st->blocks = fs->blocks_count - overhead;
	st->bfree = sb_free_blocks(fs);
	st->bavail = st->bfree > r_blocks ? st->bfree - r_blocks : 0;
	st->files = fs->sb->s_inodes_count;
	st->ffree = fs->sb->s_free_inodes_count;
	st->namemax = EXT4_NAME_LEN;
	pthread_mutex_unlock(&fs->lock);
	return 0;
}
