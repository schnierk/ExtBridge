/*
 * ExtBridge - ext2/3/4 for macOS
 * On-disk structures of ext4 and jbd2.
 *
 * Layouts follow the Linux kernel documentation
 * (Documentation/filesystems/ext4/) and e2fsprogs. ext4 is little endian,
 * jbd2 (the journal) is big endian. The code assumes a little endian host
 * (all Apple Silicon / Intel Macs and x86/arm Linux), which is checked at
 * compile time.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef L2M_EXT4_DISK_H
#define L2M_EXT4_DISK_H

#include <stdint.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "ExtBridge requires a little endian host"
#endif

#define EXT4_SUPER_MAGIC 0xEF53
#define EXT4_SUPERBLOCK_OFFSET 1024
#define EXT4_MIN_BLOCK_SIZE 1024
#define EXT4_MAX_BLOCK_SIZE 65536

#define EXT4_ROOT_INO 2
#define EXT4_JOURNAL_INO 8
#define EXT4_GOOD_OLD_FIRST_INO 11
#define EXT4_GOOD_OLD_INODE_SIZE 128

/* s_state */
#define EXT4_VALID_FS 0x0001
#define EXT4_ERROR_FS 0x0002
#define EXT4_ORPHAN_FS 0x0004

/* compat features */
#define EXT4_FEATURE_COMPAT_DIR_PREALLOC 0x0001
#define EXT4_FEATURE_COMPAT_IMAGIC_INODES 0x0002
#define EXT4_FEATURE_COMPAT_HAS_JOURNAL 0x0004
#define EXT4_FEATURE_COMPAT_EXT_ATTR 0x0008
#define EXT4_FEATURE_COMPAT_RESIZE_INODE 0x0010
#define EXT4_FEATURE_COMPAT_DIR_INDEX 0x0020
#define EXT4_FEATURE_COMPAT_LAZY_BG 0x0040
#define EXT4_FEATURE_COMPAT_EXCLUDE_BITMAP 0x0100
#define EXT4_FEATURE_COMPAT_SPARSE_SUPER2 0x0200
#define EXT4_FEATURE_COMPAT_FAST_COMMIT 0x0400
#define EXT4_FEATURE_COMPAT_STABLE_INODES 0x0800
#define EXT4_FEATURE_COMPAT_ORPHAN_FILE 0x1000

/* ro_compat features */
#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE 0x0002
#define EXT4_FEATURE_RO_COMPAT_BTREE_DIR 0x0004
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE 0x0008
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM 0x0010
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK 0x0020
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE 0x0040
#define EXT4_FEATURE_RO_COMPAT_HAS_SNAPSHOT 0x0080
#define EXT4_FEATURE_RO_COMPAT_QUOTA 0x0100
#define EXT4_FEATURE_RO_COMPAT_BIGALLOC 0x0200
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400
#define EXT4_FEATURE_RO_COMPAT_REPLICA 0x0800
#define EXT4_FEATURE_RO_COMPAT_READONLY 0x1000
#define EXT4_FEATURE_RO_COMPAT_PROJECT 0x2000
#define EXT4_FEATURE_RO_COMPAT_SHARED_BLOCKS 0x4000
#define EXT4_FEATURE_RO_COMPAT_VERITY 0x8000
#define EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT 0x10000

/* incompat features */
#define EXT4_FEATURE_INCOMPAT_COMPRESSION 0x0001
#define EXT4_FEATURE_INCOMPAT_FILETYPE 0x0002
#define EXT4_FEATURE_INCOMPAT_RECOVER 0x0004
#define EXT4_FEATURE_INCOMPAT_JOURNAL_DEV 0x0008
#define EXT4_FEATURE_INCOMPAT_META_BG 0x0010
#define EXT4_FEATURE_INCOMPAT_EXTENTS 0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT 0x0080
#define EXT4_FEATURE_INCOMPAT_MMP 0x0100
#define EXT4_FEATURE_INCOMPAT_FLEX_BG 0x0200
#define EXT4_FEATURE_INCOMPAT_EA_INODE 0x0400
#define EXT4_FEATURE_INCOMPAT_DIRDATA 0x1000
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED 0x2000
#define EXT4_FEATURE_INCOMPAT_LARGEDIR 0x4000
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA 0x8000
#define EXT4_FEATURE_INCOMPAT_ENCRYPT 0x10000
#define EXT4_FEATURE_INCOMPAT_CASEFOLD 0x20000

/* s_flags */
#define EXT2_FLAGS_SIGNED_HASH 0x0001
#define EXT2_FLAGS_UNSIGNED_HASH 0x0002
#define EXT2_FLAGS_TEST_FILESYS 0x0004

/* hash versions */
#define EXT4_HASH_LEGACY 0
#define EXT4_HASH_HALF_MD4 1
#define EXT4_HASH_TEA 2
#define EXT4_HASH_LEGACY_UNSIGNED 3
#define EXT4_HASH_HALF_MD4_UNSIGNED 4
#define EXT4_HASH_TEA_UNSIGNED 5
#define EXT4_HASH_SIPHASH 6

#define EXT4_CRC32C_CHKSUM 1

struct ext4_super_block {
	uint32_t s_inodes_count;          /* 0x00 */
	uint32_t s_blocks_count_lo;
	uint32_t s_r_blocks_count_lo;
	uint32_t s_free_blocks_count_lo;
	uint32_t s_free_inodes_count;     /* 0x10 */
	uint32_t s_first_data_block;
	uint32_t s_log_block_size;
	uint32_t s_log_cluster_size;
	uint32_t s_blocks_per_group;      /* 0x20 */
	uint32_t s_clusters_per_group;
	uint32_t s_inodes_per_group;
	uint32_t s_mtime;
	uint32_t s_wtime;                 /* 0x30 */
	uint16_t s_mnt_count;
	int16_t s_max_mnt_count;
	uint16_t s_magic;
	uint16_t s_state;
	uint16_t s_errors;
	uint16_t s_minor_rev_level;
	uint32_t s_lastcheck;             /* 0x40 */
	uint32_t s_checkinterval;
	uint32_t s_creator_os;
	uint32_t s_rev_level;
	uint16_t s_def_resuid;            /* 0x50 */
	uint16_t s_def_resgid;
	uint32_t s_first_ino;
	uint16_t s_inode_size;
	uint16_t s_block_group_nr;
	uint32_t s_feature_compat;
	uint32_t s_feature_incompat;      /* 0x60 */
	uint32_t s_feature_ro_compat;
	uint8_t s_uuid[16];               /* 0x68 */
	char s_volume_name[16];           /* 0x78 */
	char s_last_mounted[64];          /* 0x88 */
	uint32_t s_algorithm_usage_bitmap;/* 0xC8 */
	uint8_t s_prealloc_blocks;
	uint8_t s_prealloc_dir_blocks;
	uint16_t s_reserved_gdt_blocks;
	uint8_t s_journal_uuid[16];       /* 0xD0 */
	uint32_t s_journal_inum;          /* 0xE0 */
	uint32_t s_journal_dev;
	uint32_t s_last_orphan;
	uint32_t s_hash_seed[4];          /* 0xEC */
	uint8_t s_def_hash_version;       /* 0xFC */
	uint8_t s_jnl_backup_type;
	uint16_t s_desc_size;
	uint32_t s_default_mount_opts;    /* 0x100 */
	uint32_t s_first_meta_bg;
	uint32_t s_mkfs_time;
	uint32_t s_jnl_blocks[17];
	uint32_t s_blocks_count_hi;       /* 0x150 */
	uint32_t s_r_blocks_count_hi;
	uint32_t s_free_blocks_count_hi;
	uint16_t s_min_extra_isize;
	uint16_t s_want_extra_isize;
	uint32_t s_flags;                 /* 0x160 */
	uint16_t s_raid_stride;
	uint16_t s_mmp_update_interval;
	uint64_t s_mmp_block;
	uint32_t s_raid_stripe_width;     /* 0x170 */
	uint8_t s_log_groups_per_flex;
	uint8_t s_checksum_type;
	uint8_t s_encryption_level;
	uint8_t s_reserved_pad;
	uint64_t s_kbytes_written;        /* 0x178 */
	uint32_t s_snapshot_inum;         /* 0x180 */
	uint32_t s_snapshot_id;
	uint64_t s_snapshot_r_blocks_count;
	uint32_t s_snapshot_list;         /* 0x190 */
	uint32_t s_error_count;
	uint32_t s_first_error_time;
	uint32_t s_first_error_ino;
	uint64_t s_first_error_block;     /* 0x1A0 */
	uint8_t s_first_error_func[32];
	uint32_t s_first_error_line;      /* 0x1C8 */
	uint32_t s_last_error_time;
	uint32_t s_last_error_ino;        /* 0x1D0 */
	uint32_t s_last_error_line;
	uint64_t s_last_error_block;
	uint8_t s_last_error_func[32];    /* 0x1E0 */
	uint8_t s_mount_opts[64];         /* 0x200 */
	uint32_t s_usr_quota_inum;        /* 0x240 */
	uint32_t s_grp_quota_inum;
	uint32_t s_overhead_clusters;
	uint32_t s_backup_bgs[2];
	uint8_t s_encrypt_algos[4];
	uint8_t s_encrypt_pw_salt[16];
	uint32_t s_lpf_ino;               /* 0x268 */
	uint32_t s_prj_quota_inum;
	uint32_t s_checksum_seed;         /* 0x270 */
	uint8_t s_wtime_hi;
	uint8_t s_mtime_hi;
	uint8_t s_mkfs_time_hi;
	uint8_t s_lastcheck_hi;
	uint8_t s_first_error_time_hi;
	uint8_t s_last_error_time_hi;
	uint8_t s_first_error_errcode;
	uint8_t s_last_error_errcode;
	uint16_t s_encoding;              /* 0x27C */
	uint16_t s_encoding_flags;
	uint32_t s_orphan_file_inum;      /* 0x280 */
	uint32_t s_reserved[94];
	uint32_t s_checksum;              /* 0x3FC */
};
_Static_assert(sizeof(struct ext4_super_block) == 1024, "superblock size");

struct ext4_group_desc {
	uint32_t bg_block_bitmap_lo;
	uint32_t bg_inode_bitmap_lo;
	uint32_t bg_inode_table_lo;
	uint16_t bg_free_blocks_count_lo;
	uint16_t bg_free_inodes_count_lo;
	uint16_t bg_used_dirs_count_lo;
	uint16_t bg_flags;
	uint32_t bg_exclude_bitmap_lo;
	uint16_t bg_block_bitmap_csum_lo;
	uint16_t bg_inode_bitmap_csum_lo;
	uint16_t bg_itable_unused_lo;
	uint16_t bg_checksum;
	/* 64 bit part, valid if desc_size >= 64 */
	uint32_t bg_block_bitmap_hi;
	uint32_t bg_inode_bitmap_hi;
	uint32_t bg_inode_table_hi;
	uint16_t bg_free_blocks_count_hi;
	uint16_t bg_free_inodes_count_hi;
	uint16_t bg_used_dirs_count_hi;
	uint16_t bg_itable_unused_hi;
	uint32_t bg_exclude_bitmap_hi;
	uint16_t bg_block_bitmap_csum_hi;
	uint16_t bg_inode_bitmap_csum_hi;
	uint32_t bg_reserved;
};
_Static_assert(sizeof(struct ext4_group_desc) == 64, "group desc size");

#define EXT4_DESC_SIZE_32 32
#define EXT4_BG_INODE_UNINIT 0x0001
#define EXT4_BG_BLOCK_UNINIT 0x0002
#define EXT4_BG_INODE_ZEROED 0x0004

struct ext4_inode {
	uint16_t i_mode;          /* 0x00 */
	uint16_t i_uid;
	uint32_t i_size_lo;
	uint32_t i_atime;         /* 0x08 */
	uint32_t i_ctime;
	uint32_t i_mtime;         /* 0x10 */
	uint32_t i_dtime;
	uint16_t i_gid;           /* 0x18 */
	uint16_t i_links_count;
	uint32_t i_blocks_lo;
	uint32_t i_flags;         /* 0x20 */
	uint32_t i_version;       /* osd1 */
	uint32_t i_block[15];     /* 0x28 */
	uint32_t i_generation;    /* 0x64 */
	uint32_t i_file_acl_lo;
	uint32_t i_size_high;
	uint32_t i_obso_faddr;
	/* osd2 (linux) */
	uint16_t i_blocks_high;   /* 0x74 */
	uint16_t i_file_acl_high;
	uint16_t i_uid_high;
	uint16_t i_gid_high;
	uint16_t i_checksum_lo;   /* 0x7C */
	uint16_t i_reserved;
	/* large inode part */
	uint16_t i_extra_isize;   /* 0x80 */
	uint16_t i_checksum_hi;
	uint32_t i_ctime_extra;
	uint32_t i_mtime_extra;
	uint32_t i_atime_extra;
	uint32_t i_crtime;
	uint32_t i_crtime_extra;
	uint32_t i_version_hi;
	uint32_t i_projid;        /* 0x9C */
};
_Static_assert(sizeof(struct ext4_inode) == 160, "inode size");

#define EXT4_INODE_CSUM_HI_EXTRA_END 4 /* i_extra_isize >= 4 -> i_checksum_hi valid */

/* i_flags */
#define EXT4_SECRM_FL 0x00000001
#define EXT4_UNRM_FL 0x00000002
#define EXT4_COMPR_FL 0x00000004
#define EXT4_SYNC_FL 0x00000008
#define EXT4_IMMUTABLE_FL 0x00000010
#define EXT4_APPEND_FL 0x00000020
#define EXT4_NODUMP_FL 0x00000040
#define EXT4_NOATIME_FL 0x00000080
#define EXT4_ENCRYPT_FL 0x00000800
#define EXT4_INDEX_FL 0x00001000
#define EXT4_JOURNAL_DATA_FL 0x00004000
#define EXT4_HUGE_FILE_FL 0x00040000
#define EXT4_EXTENTS_FL 0x00080000
#define EXT4_VERITY_FL 0x00100000
#define EXT4_EA_INODE_FL 0x00200000
#define EXT4_INLINE_DATA_FL 0x10000000
#define EXT4_PROJINHERIT_FL 0x20000000
#define EXT4_CASEFOLD_FL 0x40000000

/* modes */
#define L2M_S_IFMT 0170000
#define L2M_S_IFSOCK 0140000
#define L2M_S_IFLNK 0120000
#define L2M_S_IFREG 0100000
#define L2M_S_IFBLK 0060000
#define L2M_S_IFDIR 0040000
#define L2M_S_IFCHR 0020000
#define L2M_S_IFIFO 0010000

/* extents */
#define EXT4_EXT_MAGIC 0xF30A
struct ext4_extent_header {
	uint16_t eh_magic;
	uint16_t eh_entries;
	uint16_t eh_max;
	uint16_t eh_depth;
	uint32_t eh_generation;
};
struct ext4_extent {
	uint32_t ee_block;
	uint16_t ee_len;
	uint16_t ee_start_hi;
	uint32_t ee_start_lo;
};
struct ext4_extent_idx {
	uint32_t ei_block;
	uint32_t ei_leaf_lo;
	uint16_t ei_leaf_hi;
	uint16_t ei_unused;
};
struct ext4_extent_tail {
	uint32_t et_checksum;
};
#define EXT4_EXT_INIT_MAX_LEN 32768u
#define EXT4_EXT_UNWRITTEN_MAX_LEN (32768u - 1)
#define EXT4_EXT_MAX_DEPTH 5

/* directories */
struct ext4_dir_entry_2 {
	uint32_t inode;
	uint16_t rec_len;
	uint8_t name_len;
	uint8_t file_type;
	char name[];
};
#define EXT4_NAME_LEN 255
#define EXT4_DIR_REC_LEN(n) ((((n) + 8) + 3) & ~3u)
#define EXT4_FT_UNKNOWN 0
#define EXT4_FT_REG_FILE 1
#define EXT4_FT_DIR 2
#define EXT4_FT_CHRDEV 3
#define EXT4_FT_BLKDEV 4
#define EXT4_FT_FIFO 5
#define EXT4_FT_SOCK 6
#define EXT4_FT_SYMLINK 7
#define EXT4_FT_DIR_CSUM 0xDE

struct ext4_dir_entry_tail {
	uint32_t det_reserved_zero1;
	uint16_t det_rec_len; /* 12 */
	uint8_t det_reserved_zero2;
	uint8_t det_reserved_ft; /* 0xDE */
	uint32_t det_checksum;
};

/* htree */
struct dx_root_info {
	uint32_t reserved_zero;
	uint8_t hash_version;
	uint8_t info_length; /* 8 */
	uint8_t indirect_levels;
	uint8_t unused_flags;
};
struct dx_entry {
	uint32_t hash;
	uint32_t block;
};
struct dx_countlimit {
	uint16_t limit;
	uint16_t count;
};
struct dx_tail {
	uint32_t dt_reserved;
	uint32_t dt_checksum;
};

/* extended attributes */
#define EXT4_XATTR_MAGIC 0xEA020000
struct ext4_xattr_header {
	uint32_t h_magic;
	uint32_t h_refcount;
	uint32_t h_blocks;
	uint32_t h_hash;
	uint32_t h_checksum;
	uint32_t h_reserved[3];
};
struct ext4_xattr_ibody_header {
	uint32_t h_magic;
};
struct ext4_xattr_entry {
	uint8_t e_name_len;
	uint8_t e_name_index;
	uint16_t e_value_offs;
	uint32_t e_value_inum;
	uint32_t e_value_size;
	uint32_t e_hash;
	char e_name[];
};
#define EXT4_XATTR_PAD 4
#define EXT4_XATTR_ROUND (EXT4_XATTR_PAD - 1)
#define EXT4_XATTR_LEN(n) (((n) + EXT4_XATTR_ROUND + sizeof(struct ext4_xattr_entry)) & ~EXT4_XATTR_ROUND)
#define EXT4_XATTR_SIZE(s) (((s) + EXT4_XATTR_ROUND) & ~EXT4_XATTR_ROUND)
#define EXT4_XATTR_INDEX_USER 1
#define EXT4_XATTR_INDEX_POSIX_ACL_ACCESS 2
#define EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT 3
#define EXT4_XATTR_INDEX_TRUSTED 4
#define EXT4_XATTR_INDEX_LUSTRE 5
#define EXT4_XATTR_INDEX_SECURITY 6
#define EXT4_XATTR_INDEX_SYSTEM 7
#define EXT4_XATTR_INDEX_RICHACL 8

/* ---- jbd2 (big endian on disk) ---- */
#define JBD2_MAGIC_NUMBER 0xC03B3998U
#define JBD2_DESCRIPTOR_BLOCK 1
#define JBD2_COMMIT_BLOCK 2
#define JBD2_SUPERBLOCK_V1 3
#define JBD2_SUPERBLOCK_V2 4
#define JBD2_REVOKE_BLOCK 5
#define JBD2_FC_BLOCK 6

struct jbd2_header {
	uint32_t h_magic;
	uint32_t h_blocktype;
	uint32_t h_sequence;
};

#define JBD2_FEATURE_COMPAT_CHECKSUM 0x1
#define JBD2_FEATURE_INCOMPAT_REVOKE 0x1
#define JBD2_FEATURE_INCOMPAT_64BIT 0x2
#define JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT 0x4
#define JBD2_FEATURE_INCOMPAT_CSUM_V2 0x8
#define JBD2_FEATURE_INCOMPAT_CSUM_V3 0x10
#define JBD2_FEATURE_INCOMPAT_FAST_COMMIT 0x20

struct jbd2_superblock {
	struct jbd2_header s_header;
	uint32_t s_blocksize;
	uint32_t s_maxlen;
	uint32_t s_first;
	uint32_t s_sequence;      /* 0x18 */
	uint32_t s_start;
	int32_t s_errno;
	uint32_t s_feature_compat;
	uint32_t s_feature_incompat;
	uint32_t s_feature_ro_compat;
	uint8_t s_uuid[16];       /* 0x30 */
	uint32_t s_nr_users;      /* 0x40 */
	uint32_t s_dynsuper;
	uint32_t s_max_transaction;
	uint32_t s_max_trans_data;
	uint8_t s_checksum_type;  /* 0x50 */
	uint8_t s_padding2[3];
	uint32_t s_num_fc_blks;
	uint32_t s_head;
	uint32_t s_padding[40];
	uint32_t s_checksum;      /* 0xFC */
	uint8_t s_users[16 * 48]; /* 0x100 */
};
_Static_assert(sizeof(struct jbd2_superblock) == 1024, "jbd2 sb size");

#define JBD2_FLAG_ESCAPE 1
#define JBD2_FLAG_SAME_UUID 2
#define JBD2_FLAG_DELETED 4
#define JBD2_FLAG_LAST_TAG 8

struct jbd2_block_tag3 {
	uint32_t t_blocknr;
	uint32_t t_flags;
	uint32_t t_blocknr_high;
	uint32_t t_checksum;
};
struct jbd2_block_tag {
	uint32_t t_blocknr;
	uint16_t t_checksum;
	uint16_t t_flags;
	uint32_t t_blocknr_high;
};
struct jbd2_block_tail {
	uint32_t t_checksum;
};
struct jbd2_commit_header {
	uint32_t h_magic;
	uint32_t h_blocktype;
	uint32_t h_sequence;
	uint8_t h_chksum_type;
	uint8_t h_chksum_size;
	uint8_t h_padding[2];
	uint32_t h_chksum[8];
	uint64_t h_commit_sec;
	uint32_t h_commit_nsec;
};
struct jbd2_revoke_header {
	struct jbd2_header r_header;
	uint32_t r_count;
};

#define JBD2_CRC32C_CHKSUM 4

static inline uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t be64(uint64_t v) { return __builtin_bswap64(v); }
static inline uint16_t be16(uint16_t v) { return __builtin_bswap16(v); }

#endif
