/*
 * ExtBridge - block mapping: ext4 extent trees (read/write) and the
 * legacy ext2/3 indirect block map (read only).
 *
 * Tree invariants that are maintained (and checked by e2fsck):
 *  - entries of every node are sorted and do not overlap
 *  - an index entry's ei_block equals the first logical block of its child
 *  - only the root may be empty; an empty root has depth 0
 *  - every tree block carries an ext4_extent_tail checksum (metadata_csum)
 *
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

#define ROOT_MAX 4u
#define MAX_INIT_LEN EXT4_EXT_INIT_MAX_LEN
#define MAX_UNWRITTEN_LEN EXT4_EXT_UNWRITTEN_MAX_LEN

struct epath {
	struct l2m_buf *buf; /* NULL for the root in the inode */
	struct ext4_extent_header *hdr;
	int pos;
};

static inline uint32_t node_max(l2m_fs *fs) { return (fs->bs - sizeof(struct ext4_extent_header)) / sizeof(struct ext4_extent); }
static inline struct ext4_extent_header *root_hdr(struct l2m_inode *in) { return (struct ext4_extent_header *)in->i.i_block; }
static inline struct ext4_extent *ext_at(struct ext4_extent_header *h, int i) { return (struct ext4_extent *)(h + 1) + i; }
static inline struct ext4_extent_idx *idx_at(struct ext4_extent_header *h, int i) { return (struct ext4_extent_idx *)(h + 1) + i; }
static inline uint64_t ext_pblk(const struct ext4_extent *e) { return e->ee_start_lo | ((uint64_t)e->ee_start_hi << 32); }
static inline uint64_t idx_pblk(const struct ext4_extent_idx *x) { return x->ei_leaf_lo | ((uint64_t)x->ei_leaf_hi << 32); }
static inline bool ext_unwritten(const struct ext4_extent *e) { return e->ee_len > MAX_INIT_LEN; }
static inline uint32_t ext_len(const struct ext4_extent *e) { return e->ee_len > MAX_INIT_LEN ? e->ee_len - MAX_INIT_LEN : e->ee_len; }
static inline void ext_set_pblk(struct ext4_extent *e, uint64_t p)
{
	e->ee_start_lo = (uint32_t)p;
	e->ee_start_hi = (uint16_t)(p >> 32);
}
static inline void ext_set_len(struct ext4_extent *e, uint32_t len, bool unwritten)
{
	e->ee_len = (uint16_t)(unwritten ? len + MAX_INIT_LEN : len);
}
static inline void idx_set_pblk(struct ext4_extent_idx *x, uint64_t p)
{
	x->ei_leaf_lo = (uint32_t)p;
	x->ei_leaf_hi = (uint16_t)(p >> 32);
	x->ei_unused = 0;
}

uint32_t ext_tail_csum(l2m_fs *fs, struct l2m_inode *in, const uint8_t *block)
{
	const struct ext4_extent_header *h = (const void *)block;
	size_t off = sizeof(*h) + (size_t)h->eh_max * sizeof(struct ext4_extent);
	if (off + 4 > fs->bs)
		off = fs->bs - 4;
	return l2m_crc32c(in->csum_seed, block, off);
}

static int check_hdr(l2m_fs *fs, struct ext4_extent_header *h, bool root, int depth, uint64_t blk)
{
	uint32_t max = root ? ROOT_MAX : node_max(fs);
	if (h->eh_magic != EXT4_EXT_MAGIC)
		return l2m_corrupt(fs, "extent header magic wrong", blk);
	if (h->eh_entries > h->eh_max || h->eh_max > max || h->eh_max == 0)
		return l2m_corrupt(fs, "extent header entries invalid", blk);
	if (h->eh_depth > EXT4_EXT_MAX_DEPTH || (depth >= 0 && h->eh_depth != depth))
		return l2m_corrupt(fs, "extent tree depth invalid", blk);
	return 0;
}

static int node_dirty(l2m_fs *fs, struct l2m_inode *in, struct epath *p)
{
	if (!p->buf)
		return 0; /* root: caller writes the inode */
	if (fs->has_csum) {
		struct ext4_extent_header *h = p->hdr;
		size_t off = sizeof(*h) + (size_t)h->eh_max * sizeof(struct ext4_extent);
		uint32_t c = ext_tail_csum(fs, in, p->buf->data);
		memcpy(p->buf->data + off, &c, 4);
	}
	return buf_dirty(fs, p->buf);
}

static void path_release(l2m_fs *fs, struct epath *path, int depth)
{
	for (int i = 0; i <= depth && i <= EXT4_EXT_MAX_DEPTH; i++) {
		if (path[i].buf)
			buf_put(fs, path[i].buf);
		path[i].buf = NULL;
	}
}

static int read_node(l2m_fs *fs, struct l2m_inode *in, uint64_t blk, int depth, struct epath *p)
{
	if (blk < fs->first_data_block || blk >= fs->blocks_count)
		return l2m_corrupt(fs, "extent node block out of range", blk);
	int r = buf_get(fs, blk, &p->buf);
	if (r)
		return r;
	p->hdr = (struct ext4_extent_header *)p->buf->data;
	r = check_hdr(fs, p->hdr, false, depth, blk);
	if (!r && fs->has_csum && fs->verify_csums && !p->buf->dirty) {
		struct ext4_extent_header *h = p->hdr;
		size_t off = sizeof(*h) + (size_t)h->eh_max * sizeof(struct ext4_extent);
		if (le32_at(p->buf->data + off) != ext_tail_csum(fs, in, p->buf->data)) {
			l2m_log(fs, L2M_LOG_ERROR, "extent block %llu of inode %u: checksum mismatch", (unsigned long long)blk, in->ino);
			r = -EFSBADCRC;
		}
	}
	if (r) {
		buf_put(fs, p->buf);
		p->buf = NULL;
	}
	return r;
}

/* Walk down to the leaf for lblk. path[d].pos: index entry taken; for the
 * leaf: last extent with ee_block <= lblk, or -1. Returns depth or -err. */
static int ext_find(l2m_fs *fs, struct l2m_inode *in, uint64_t lblk, struct epath *path)
{
	struct ext4_extent_header *h = root_hdr(in);
	int r = check_hdr(fs, h, true, -1, in->ino);
	if (r)
		return r;
	int depth = h->eh_depth;
	memset(path, 0, sizeof(*path) * (EXT4_EXT_MAX_DEPTH + 1));
	path[0].hdr = h;
	for (int lvl = 0;; lvl++) {
		h = path[lvl].hdr;
		int lo = 0, hi = (int)h->eh_entries - 1, found = -1;
		if (lvl < depth) {
			if (h->eh_entries == 0) {
				path_release(fs, path, depth);
				return l2m_corrupt(fs, "empty extent index node", in->ino);
			}
			while (lo <= hi) {
				int mid = (lo + hi) / 2;
				if (idx_at(h, mid)->ei_block <= lblk)
					found = mid, lo = mid + 1;
				else
					hi = mid - 1;
			}
			if (found < 0)
				found = 0;
			path[lvl].pos = found;
			r = read_node(fs, in, idx_pblk(idx_at(h, found)), depth - lvl - 1, &path[lvl + 1]);
			if (r) {
				path_release(fs, path, depth);
				return r;
			}
		} else {
			while (lo <= hi) {
				int mid = (lo + hi) / 2;
				if (ext_at(h, mid)->ee_block <= lblk)
					found = mid, lo = mid + 1;
				else
					hi = mid - 1;
			}
			path[lvl].pos = found;
			return depth;
		}
	}
}

/* first logical block after the subtree of the leaf in path (or max) */
static uint64_t next_allocated(struct epath *path, int depth)
{
	struct ext4_extent_header *leaf = path[depth].hdr;
	int p = path[depth].pos;
	if (p + 1 < leaf->eh_entries)
		return ext_at(leaf, p + 1)->ee_block;
	for (int lvl = depth - 1; lvl >= 0; lvl--)
		if (path[lvl].pos + 1 < path[lvl].hdr->eh_entries)
			return idx_at(path[lvl].hdr, path[lvl].pos + 1)->ei_block;
	return 0x100000000ull; /* 2^32: beyond the largest logical block */
}

/* ---- legacy indirect map (read only) ------------------------------------ */

static int ind_get(l2m_fs *fs, uint64_t blk, uint32_t idx, uint32_t *out)
{
	struct l2m_buf *b;
	if (!blk) {
		*out = 0;
		return 0;
	}
	int r = buf_get(fs, blk, &b);
	if (r)
		return r;
	*out = ((uint32_t *)b->data)[idx];
	buf_put(fs, b);
	return 0;
}

static int ind_bmap(l2m_fs *fs, struct l2m_inode *in, uint64_t lblk, struct l2m_map *m)
{
	uint32_t apb = fs->bs / 4;
	uint32_t p = 0;
	int r = 0;
	memset(m, 0, sizeof(*m));
	m->lblk = lblk;
	m->len = 1;
	if (lblk < 12) {
		p = in->i.i_block[lblk];
	} else if (lblk < 12 + (uint64_t)apb) {
		r = ind_get(fs, in->i.i_block[12], (uint32_t)(lblk - 12), &p);
	} else if (lblk < 12 + (uint64_t)apb + (uint64_t)apb * apb) {
		uint64_t x = lblk - 12 - apb;
		uint32_t a;
		r = ind_get(fs, in->i.i_block[13], (uint32_t)(x / apb), &a);
		if (!r)
			r = ind_get(fs, a, (uint32_t)(x % apb), &p);
	} else {
		uint64_t x = lblk - 12 - apb - (uint64_t)apb * apb;
		uint32_t a, b2;
		if (x >= (uint64_t)apb * apb * apb)
			return -EFBIG;
		r = ind_get(fs, in->i.i_block[14], (uint32_t)(x / ((uint64_t)apb * apb)), &a);
		if (!r)
			r = ind_get(fs, a, (uint32_t)((x / apb) % apb), &b2);
		if (!r)
			r = ind_get(fs, b2, (uint32_t)(x % apb), &p);
	}
	if (r)
		return r;
	if (p >= fs->blocks_count)
		return l2m_corrupt(fs, "indirect block pointer out of range", in->ino);
	m->pblk = p;
	return 0;
}

/* ---- public mapping ---------------------------------------------------- */

int bmap(l2m_fs *fs, struct l2m_inode *in, uint64_t lblk, struct l2m_map *m)
{
	memset(m, 0, sizeof(*m));
	m->lblk = lblk;
	if (in->i.i_flags & EXT4_INLINE_DATA_FL) {
		m->len = 1;
		return 0;
	}
	if (!(in->i.i_flags & EXT4_EXTENTS_FL)) {
		int r = ind_bmap(fs, in, lblk, m);
		if (r)
			return r;
		/* extend contiguous runs for faster reads */
		while (m->len < 1024) {
			struct l2m_map n;
			if (ind_bmap(fs, in, lblk + m->len, &n))
				break;
			if ((m->pblk == 0) != (n.pblk == 0) || (m->pblk && n.pblk != m->pblk + m->len))
				break;
			m->len++;
		}
		return 0;
	}
	struct epath path[EXT4_EXT_MAX_DEPTH + 1];
	int depth = ext_find(fs, in, lblk, path);
	if (depth < 0)
		return depth;
	struct ext4_extent_header *leaf = path[depth].hdr;
	int p = path[depth].pos;
	if (p >= 0) {
		struct ext4_extent *e = ext_at(leaf, p);
		uint32_t len = ext_len(e);
		if (lblk < (uint64_t)e->ee_block + len) {
			uint64_t pb = ext_pblk(e) + (lblk - e->ee_block);
			if (pb + (e->ee_block + len - lblk) > fs->blocks_count) {
				path_release(fs, path, depth);
				return l2m_corrupt(fs, "extent points beyond end of file system", in->ino);
			}
			m->pblk = pb;
			m->len = (uint32_t)(e->ee_block + len - lblk);
			m->unwritten = ext_unwritten(e);
			path_release(fs, path, depth);
			return 0;
		}
	}
	uint64_t next = next_allocated(path, depth);
	path_release(fs, path, depth);
	uint64_t hole = next > lblk ? next - lblk : 1;
	m->len = (uint32_t)L2M_MIN(hole, (uint64_t)UINT32_MAX);
	return 0;
}

/* ---- tree modification ---------------------------------------------------- */

static int correct_indexes(l2m_fs *fs, struct l2m_inode *in, struct epath *path, int lvl)
{
	/* the first entry of node lvl changed: propagate its start upward */
	while (lvl > 0) {
		struct ext4_extent_header *h = path[lvl].hdr;
		uint32_t first = h->eh_depth == 0 ? ext_at(h, 0)->ee_block : idx_at(h, 0)->ei_block;
		struct epath *pp = &path[lvl - 1];
		struct ext4_extent_idx *x = idx_at(pp->hdr, pp->pos);
		if (x->ei_block == first)
			break;
		x->ei_block = first;
		int r = node_dirty(fs, in, pp);
		if (r)
			return r;
		if (pp->pos != 0)
			break;
		lvl--;
	}
	return 0;
}

static uint64_t node_block(struct epath *p) { return p->buf ? p->buf->blk : 0; }

static int alloc_tree_block(l2m_fs *fs, struct l2m_inode *in, uint64_t goal, uint64_t *blk)
{
	uint32_t got;
	if (!goal)
		goal = goal_for_inode(fs, in->ino);
	int r = alloc_blocks(fs, goal, 1, blk, &got, true);
	if (r)
		return r;
	inode_add_blocks(fs, in, 1);
	return 0;
}

/* Grow the tree by one level: root entries move into a new block. */
static int grow_root(l2m_fs *fs, struct l2m_inode *in)
{
	struct ext4_extent_header *rh = root_hdr(in);
	if (rh->eh_depth >= EXT4_EXT_MAX_DEPTH)
		return -EFBIG;
	uint64_t nb;
	int r = alloc_tree_block(fs, in, 0, &nb);
	if (r)
		return r;
	struct epath np = {0};
	r = buf_get_new(fs, nb, &np.buf);
	if (r)
		return r;
	np.hdr = (struct ext4_extent_header *)np.buf->data;
	memcpy(np.hdr, rh, sizeof(*rh) + rh->eh_entries * sizeof(struct ext4_extent));
	np.hdr->eh_max = (uint16_t)node_max(fs);
	r = node_dirty(fs, in, &np);
	uint32_t first = rh->eh_entries ? (rh->eh_depth == 0 ? ext_at(rh, 0)->ee_block : idx_at(rh, 0)->ei_block) : 0;
	buf_put(fs, np.buf);
	if (r)
		return r;
	rh->eh_depth++;
	rh->eh_entries = 1;
	idx_at(rh, 0)->ei_block = first;
	idx_set_pblk(idx_at(rh, 0), nb);
	return 0;
}

/* Split the full node at level lvl (its parent has room). ins_pos is the
 * leaf insertion position when lvl is the leaf. */
static int split_node(l2m_fs *fs, struct l2m_inode *in, struct epath *path, int lvl, int depth, int ins_pos, uint64_t lblk)
{
	struct ext4_extent_header *h = path[lvl].hdr;
	int n = h->eh_entries, s;
	if (lvl == depth)
		s = ins_pos;
	else
		s = (path[lvl].pos + 1 < n) ? path[lvl].pos + 1 : path[lvl].pos;
	if (s < 0)
		s = 0;
	if (s > n)
		s = n;
	if (s == n && h->eh_depth == 0 && n > 0) {
		/* an empty new leaf starts at lblk: that must be behind the old
		 * leaf's last extent, or the tree would be inconsistent */
		struct ext4_extent *l = ext_at(h, n - 1);
		if ((uint64_t)l->ee_block + ext_len(l) > lblk)
			return l2m_corrupt(fs, "new extent leaf would start inside an extent", in->ino);
	}
	uint64_t nb;
	int r = alloc_tree_block(fs, in, node_block(&path[lvl]) + 1, &nb);
	if (r)
		return r;
	struct epath np = {0};
	r = buf_get_new(fs, nb, &np.buf);
	if (r)
		return r;
	np.hdr = (struct ext4_extent_header *)np.buf->data;
	np.hdr->eh_magic = EXT4_EXT_MAGIC;
	np.hdr->eh_max = (uint16_t)node_max(fs);
	np.hdr->eh_depth = h->eh_depth;
	np.hdr->eh_entries = (uint16_t)(n - s);
	memcpy(ext_at(np.hdr, 0), ext_at(h, s), (size_t)(n - s) * sizeof(struct ext4_extent));
	uint32_t first;
	if (n - s > 0)
		first = h->eh_depth == 0 ? ext_at(h, s)->ee_block : idx_at(h, s)->ei_block;
	else
		first = (uint32_t)lblk;
	r = node_dirty(fs, in, &np);
	buf_put(fs, np.buf);
	if (r)
		return r;
	h->eh_entries = (uint16_t)s;
	r = node_dirty(fs, in, &path[lvl]);
	if (r)
		return r;
	/* new index entry in the parent right after the current one */
	struct epath *pp = &path[lvl - 1];
	struct ext4_extent_header *ph = pp->hdr;
	int at = pp->pos + 1;
	memmove(idx_at(ph, at + 1), idx_at(ph, at), (size_t)(ph->eh_entries - at) * sizeof(struct ext4_extent_idx));
	idx_at(ph, at)->ei_block = first;
	idx_set_pblk(idx_at(ph, at), nb);
	ph->eh_entries++;
	return node_dirty(fs, in, pp);
}

/* One step of making room in the leaf of path. A full leaf is split at
 * split_pos: entries from there on move to a new leaf. When nothing moves,
 * the new leaf starts at lblk, which must then lie behind every extent of
 * the old leaf (a hole being filled). */
static int make_room(l2m_fs *fs, struct l2m_inode *in, struct epath *path, int depth, int split_pos, uint64_t lblk)
{
	int k;
	for (k = depth - 1; k >= 0; k--)
		if (path[k].hdr->eh_entries < path[k].hdr->eh_max)
			break;
	if (k < 0)
		return grow_root(fs, in);
	return split_node(fs, in, path, k + 1, depth, split_pos, lblk);
}

static bool can_merge(const struct ext4_extent *a, uint64_t lblk, uint64_t pblk, uint32_t len, bool unwritten)
{
	if (ext_unwritten(a) != unwritten)
		return false;
	uint32_t max = unwritten ? MAX_UNWRITTEN_LEN : MAX_INIT_LEN;
	return (uint64_t)a->ee_block + ext_len(a) == lblk && ext_pblk(a) + ext_len(a) == pblk && ext_len(a) + len <= max;
}

static int leaf_insert_at(l2m_fs *fs, struct l2m_inode *in, struct epath *path, int depth, int at, uint64_t lblk, uint64_t pblk, uint32_t len, bool unwritten)
{
	struct ext4_extent_header *h = path[depth].hdr;
	memmove(ext_at(h, at + 1), ext_at(h, at), (size_t)(h->eh_entries - at) * sizeof(struct ext4_extent));
	struct ext4_extent *e = ext_at(h, at);
	e->ee_block = (uint32_t)lblk;
	ext_set_pblk(e, pblk);
	ext_set_len(e, len, unwritten);
	h->eh_entries++;
	int r = node_dirty(fs, in, &path[depth]);
	if (!r && at == 0)
		r = correct_indexes(fs, in, path, depth);
	return r;
}

/* Insert a new extent into a hole. */
static int ext_insert(l2m_fs *fs, struct l2m_inode *in, uint64_t lblk, uint64_t pblk, uint32_t len, bool unwritten)
{
	struct epath path[EXT4_EXT_MAX_DEPTH + 1];
	if (lblk + len > 0x100000000ull)
		return -EFBIG;
	for (int iter = 0; iter < 64; iter++) {
		int depth = ext_find(fs, in, lblk, path);
		if (depth < 0)
			return depth;
		struct ext4_extent_header *h = path[depth].hdr;
		int p = path[depth].pos;
		int r;
		/* try merging with the left / right neighbour */
		if (p >= 0 && can_merge(ext_at(h, p), lblk, pblk, len, unwritten)) {
			struct ext4_extent *a = ext_at(h, p);
			ext_set_len(a, ext_len(a) + len, unwritten);
			if (p + 1 < h->eh_entries) {
				struct ext4_extent *b = ext_at(h, p + 1);
				if (can_merge(a, b->ee_block, ext_pblk(b), ext_len(b), ext_unwritten(b))) {
					ext_set_len(a, ext_len(a) + ext_len(b), unwritten);
					memmove(b, b + 1, (size_t)(h->eh_entries - p - 2) * sizeof(*b));
					h->eh_entries--;
				}
			}
			r = node_dirty(fs, in, &path[depth]);
			path_release(fs, path, depth);
			return r;
		}
		if (p + 1 < h->eh_entries) {
			struct ext4_extent *b = ext_at(h, p + 1);
			uint32_t max = unwritten ? MAX_UNWRITTEN_LEN : MAX_INIT_LEN;
			if (ext_unwritten(b) == unwritten && lblk + len == b->ee_block && pblk + len == ext_pblk(b) && ext_len(b) + len <= max) {
				b->ee_block = (uint32_t)lblk;
				ext_set_pblk(b, pblk);
				ext_set_len(b, ext_len(b) + len, unwritten);
				r = node_dirty(fs, in, &path[depth]);
				if (!r && p + 1 == 0)
					r = correct_indexes(fs, in, path, depth);
				path_release(fs, path, depth);
				return r;
			}
		}
		if (h->eh_entries < h->eh_max) {
			r = leaf_insert_at(fs, in, path, depth, p + 1, lblk, pblk, len, unwritten);
			path_release(fs, path, depth);
			return r;
		}
		r = make_room(fs, in, path, depth, p + 1, lblk);
		path_release(fs, path, depth);
		if (r)
			return r;
	}
	return l2m_corrupt(fs, "extent insert did not converge", in->ino);
}

int ext_init_root(struct l2m_inode *in)
{
	struct ext4_extent_header *h = root_hdr(in);
	memset(in->i.i_block, 0, sizeof(in->i.i_block));
	h->eh_magic = EXT4_EXT_MAGIC;
	h->eh_max = ROOT_MAX;
	h->eh_depth = 0;
	h->eh_entries = 0;
	in->i.i_flags |= EXT4_EXTENTS_FL;
	return 0;
}

uint64_t goal_for_inode(l2m_fs *fs, uint32_t ino)
{
	uint32_t g = (ino - 1) / fs->ipg;
	/* with flex_bg, data goes after the flex group's metadata */
	if (fs->has_flex && fs->sb->s_log_groups_per_flex < 31) {
		uint32_t fsz = 1u << fs->sb->s_log_groups_per_flex;
		g = (g / fsz) * fsz;
	}
	return group_first_block(fs, g);
}

int bmap_alloc(l2m_fs *fs, struct l2m_inode *in, uint64_t lblk, uint64_t count, bool unwritten, uint64_t goal, struct l2m_map *m)
{
	if (!(in->i.i_flags & EXT4_EXTENTS_FL))
		return -EOPNOTSUPP; /* writing to ext2/3 block-mapped files */
	int r = bmap(fs, in, lblk, m);
	if (r)
		return r;
	if (m->pblk)
		return 0;
	uint64_t n = L2M_MIN(count, (uint64_t)m->len);
	n = L2M_MIN(n, (uint64_t)(unwritten ? MAX_UNWRITTEN_LEN : MAX_INIT_LEN));
	if (n == 0)
		n = 1;
	if (!goal) {
		/* continue behind the previous extent */
		if (lblk > 0) {
			struct l2m_map prev;
			if (bmap(fs, in, lblk - 1, &prev) == 0 && prev.pblk)
				goal = prev.pblk + 1;
		}
		if (!goal)
			goal = goal_for_inode(fs, in->ino);
	}
	uint64_t start;
	uint32_t got;
	r = alloc_blocks(fs, goal, (uint32_t)n, &start, &got, false);
	if (r)
		return r;
	r = ext_insert(fs, in, lblk, start, got, unwritten);
	if (r) {
		free_blocks(fs, start, got);
		return r;
	}
	inode_add_blocks(fs, in, got);
	m->lblk = lblk;
	m->pblk = start;
	m->len = got;
	m->unwritten = unwritten;
	return 0;
}

/* remove the (empty) node at level lvl from its parent, recursively */
static int remove_empty_node(l2m_fs *fs, struct l2m_inode *in, struct epath *path, int lvl)
{
	while (lvl > 0 && path[lvl].hdr->eh_entries == 0) {
		uint64_t blk = path[lvl].buf->blk;
		struct epath *pp = &path[lvl - 1];
		struct ext4_extent_header *ph = pp->hdr;
		int at = pp->pos;
		memmove(idx_at(ph, at), idx_at(ph, at + 1), (size_t)(ph->eh_entries - at - 1) * sizeof(struct ext4_extent_idx));
		ph->eh_entries--;
		buf_put(fs, path[lvl].buf);
		path[lvl].buf = NULL;
		buf_forget(fs, blk);
		int r = free_blocks(fs, blk, 1);
		if (r)
			return r;
		inode_add_blocks(fs, in, -1);
		r = node_dirty(fs, in, pp);
		if (r)
			return r;
		if (ph->eh_entries && at == 0) {
			r = correct_indexes(fs, in, path, lvl - 1);
			if (r)
				return r;
		}
		lvl--;
	}
	if (root_hdr(in)->eh_entries == 0) {
		root_hdr(in)->eh_depth = 0;
		root_hdr(in)->eh_max = ROOT_MAX;
	}
	return 0;
}

static int ind_truncate(l2m_fs *fs, struct l2m_inode *in, uint64_t from);

int bmap_truncate(l2m_fs *fs, struct l2m_inode *in, uint64_t from)
{
	if (in->i.i_flags & EXT4_INLINE_DATA_FL)
		return -EOPNOTSUPP;
	if (!(in->i.i_flags & EXT4_EXTENTS_FL))
		return ind_truncate(fs, in, from);
	struct epath path[EXT4_EXT_MAX_DEPTH + 1];
	for (;;) {
		int depth = ext_find(fs, in, 0xFFFFFFFFull, path);
		if (depth < 0)
			return depth;
		struct ext4_extent_header *h = path[depth].hdr;
		int r = 0;
		if (h->eh_entries == 0) {
			if (depth == 0) {
				path_release(fs, path, depth);
				return 0;
			}
			r = remove_empty_node(fs, in, path, depth);
			path_release(fs, path, depth);
			if (r)
				return r;
			continue;
		}
		struct ext4_extent *e = ext_at(h, h->eh_entries - 1);
		uint32_t len = ext_len(e);
		if ((uint64_t)e->ee_block + len <= from) {
			path_release(fs, path, depth);
			return 0;
		}
		uint64_t pb = ext_pblk(e);
		if (e->ee_block >= from) {
			r = free_blocks(fs, pb, len);
			inode_add_blocks(fs, in, -(int64_t)len);
			h->eh_entries--;
		} else {
			uint32_t keep = (uint32_t)(from - e->ee_block);
			r = free_blocks(fs, pb + keep, len - keep);
			inode_add_blocks(fs, in, -(int64_t)(len - keep));
			ext_set_len(e, keep, ext_unwritten(e));
		}
		if (!r)
			r = node_dirty(fs, in, &path[depth]);
		if (!r && h->eh_entries == 0)
			r = remove_empty_node(fs, in, path, depth);
		path_release(fs, path, depth);
		if (r)
			return r;
		/* very large truncates: commit in between (the inode is on the
		 * orphan list, so a crash is completed by Linux/e2fsck) */
		if (fs->txn.ndirty > fs->txn_limit * 3 / 4) {
			r = inode_write(fs, in);
			if (!r)
				r = txn_commit(fs);
			if (r)
				return r;
		}
	}
}

int ext_mark_written(l2m_fs *fs, struct l2m_inode *in, uint64_t lblk, uint64_t count)
{
	struct epath path[EXT4_EXT_MAX_DEPTH + 1];
	uint64_t end = lblk + count;
	if (!(in->i.i_flags & EXT4_EXTENTS_FL))
		return 0;
	while (lblk < end) {
		int depth = ext_find(fs, in, lblk, path);
		if (depth < 0)
			return depth;
		struct ext4_extent_header *h = path[depth].hdr;
		int p = path[depth].pos;
		struct ext4_extent *e = p >= 0 ? ext_at(h, p) : NULL;
		if (!e || lblk >= (uint64_t)e->ee_block + ext_len(e)) {
			/* hole: skip to the next extent */
			uint64_t next = next_allocated(path, depth);
			path_release(fs, path, depth);
			lblk = next > lblk ? next : lblk + 1;
			continue;
		}
		uint64_t a = e->ee_block, L = ext_len(e), b = a + L;
		if (!ext_unwritten(e)) {
			path_release(fs, path, depth);
			lblk = b;
			continue;
		}
		uint64_t x = L2M_MAX(a, lblk), y = L2M_MIN(b, end);
		uint64_t pb = ext_pblk(e);
		/* The written piece joins a written neighbour in the same leaf that
		 * it touches on the disk too, as Linux does: a file written piece
		 * by piece then keeps few extents instead of one per piece. */
		struct ext4_extent *l = p > 0 ? ext_at(h, p - 1) : NULL;
		struct ext4_extent *rn = p + 1 < h->eh_entries ? ext_at(h, p + 1) : NULL;
		bool join_l = x == a && l && can_merge(l, a, pb, (uint32_t)(y - a), false);
		bool join_r = y == b && rn && !ext_unwritten(rn) && rn->ee_block == b && ext_pblk(rn) == pb + L &&
			      ext_len(rn) + (b - x) <= MAX_INIT_LEN;
		int need = (x > a) + (y < b);
		if ((x == a && y < b && join_l) || (y == b && x > a && join_r))
			need = 0; /* the neighbour grows, extent p shrinks */
		if (h->eh_entries + need > h->eh_max) {
			/* Split so that extent p keeps its leaf mates or moves along:
			 * a new leaf may never begin inside extent p (the pieces of p
			 * are inserted right after it, in the same leaf). */
			int split = p + 1 < h->eh_entries ? p + 1 : p;
			int r = make_room(fs, in, path, depth, split, a);
			path_release(fs, path, depth);
			if (r)
				return r;
			continue; /* re-find */
		}
		int r = 0;
		if (x == a && y == b) {
			ext_set_len(e, (uint32_t)L, false);
			int cur = p;
			if (join_l) {
				ext_set_len(l, ext_len(l) + (uint32_t)L, false);
				memmove(e, e + 1, (size_t)(h->eh_entries - p - 1) * sizeof(*e));
				h->eh_entries--;
				cur = p - 1;
			}
			if (cur + 1 < h->eh_entries) {
				struct ext4_extent *c = ext_at(h, cur), *n = ext_at(h, cur + 1);
				if (can_merge(c, n->ee_block, ext_pblk(n), ext_len(n), ext_unwritten(n))) {
					ext_set_len(c, ext_len(c) + ext_len(n), false);
					memmove(n, n + 1, (size_t)(h->eh_entries - cur - 2) * sizeof(*n));
					h->eh_entries--;
				}
			}
			r = node_dirty(fs, in, &path[depth]);
		} else if (x == a && join_l) {
			ext_set_len(l, ext_len(l) + (uint32_t)(y - a), false);
			e->ee_block = (uint32_t)y;
			ext_set_pblk(e, pb + (y - a));
			ext_set_len(e, (uint32_t)(b - y), true);
			r = node_dirty(fs, in, &path[depth]);
		} else if (x == a) {
			ext_set_len(e, (uint32_t)(y - a), false);
			r = leaf_insert_at(fs, in, path, depth, p + 1, y, pb + (y - a), (uint32_t)(b - y), true);
		} else if (y == b && join_r) {
			ext_set_len(e, (uint32_t)(x - a), true);
			rn->ee_block = (uint32_t)x;
			ext_set_pblk(rn, pb + (x - a));
			ext_set_len(rn, ext_len(rn) + (uint32_t)(b - x), false);
			r = node_dirty(fs, in, &path[depth]);
		} else if (y == b) {
			ext_set_len(e, (uint32_t)(x - a), true);
			r = leaf_insert_at(fs, in, path, depth, p + 1, x, pb + (x - a), (uint32_t)(b - x), false);
		} else {
			ext_set_len(e, (uint32_t)(x - a), true);
			r = leaf_insert_at(fs, in, path, depth, p + 1, x, pb + (x - a), (uint32_t)(y - x), false);
			if (!r)
				r = leaf_insert_at(fs, in, path, depth, p + 2, y, pb + (y - a), (uint32_t)(b - y), true);
		}
		path_release(fs, path, depth);
		if (r)
			return r;
		lblk = y;
	}
	return 0;
}

/* ---- legacy indirect truncate (only needed to delete ext2/3 files) ---- */

static int ind_free_tree(l2m_fs *fs, struct l2m_inode *in, uint32_t blk, int level, uint64_t base, uint64_t from, bool *empty)
{
	/* frees everything in the subtree covering logical blocks >= from;
	 * *empty tells whether the subtree no longer maps anything */
	uint32_t apb = fs->bs / 4;
	uint64_t span = 1;
	for (int i = 0; i < level; i++)
		span *= apb;
	struct l2m_buf *b;
	int r = buf_get(fs, blk, &b);
	if (r)
		return r;
	uint32_t *p = (uint32_t *)b->data;
	bool all_empty = true, changed = false;
	for (uint32_t i = 0; i < apb; i++) {
		uint64_t lo = base + i * span;
		if (!p[i])
			continue;
		if (lo + span <= from) {
			all_empty = false;
			continue;
		}
		if (level == 0) {
			if (lo >= from) {
				r = free_blocks(fs, p[i], 1);
				inode_add_blocks(fs, in, -1);
				p[i] = 0;
				changed = true;
			} else {
				all_empty = false;
			}
		} else {
			bool e;
			r = ind_free_tree(fs, in, p[i], level - 1, lo, from, &e);
			if (!r && e) {
				buf_forget(fs, p[i]);
				r = free_blocks(fs, p[i], 1);
				inode_add_blocks(fs, in, -1);
				p[i] = 0;
				changed = true;
			} else {
				all_empty = false;
			}
		}
		if (r)
			break;
	}
	if (!r && changed && !all_empty)
		r = buf_dirty(fs, b);
	buf_put(fs, b);
	*empty = all_empty;
	return r;
}

static int ind_truncate(l2m_fs *fs, struct l2m_inode *in, uint64_t from)
{
	uint32_t apb = fs->bs / 4;
	int r = 0;
	for (uint32_t i = 0; i < 12; i++)
		if (i >= from && in->i.i_block[i]) {
			r = free_blocks(fs, in->i.i_block[i], 1);
			if (r)
				return r;
			inode_add_blocks(fs, in, -1);
			in->i.i_block[i] = 0;
		}
	uint64_t base = 12, span = apb;
	for (int lvl = 0; lvl < 3; lvl++) {
		uint32_t blk = in->i.i_block[12 + lvl];
		if (blk && base + span > from) {
			bool e;
			r = ind_free_tree(fs, in, blk, lvl, base, from, &e);
			if (r)
				return r;
			if (e) {
				buf_forget(fs, blk);
				r = free_blocks(fs, blk, 1);
				if (r)
					return r;
				inode_add_blocks(fs, in, -1);
				in->i.i_block[12 + lvl] = 0;
			}
		}
		base += span;
		span *= apb;
	}
	return 0;
}

/* ---- verification (diagnostics / tests) --------------------------------- */

static int verify_node(l2m_fs *fs, struct l2m_inode *in, struct ext4_extent_header *h, int depth, uint64_t lo, uint64_t hi, uint64_t *nblocks, uint64_t *last)
{
	for (int i = 0; i < h->eh_entries; i++) {
		if (depth == 0) {
			struct ext4_extent *e = ext_at(h, i);
			if (e->ee_block < lo || (uint64_t)e->ee_block + ext_len(e) > hi || e->ee_block < *last || ext_len(e) == 0)
				return l2m_corrupt(fs, "extent out of order", in->ino);
			if (ext_pblk(e) < fs->first_data_block || ext_pblk(e) + ext_len(e) > fs->blocks_count)
				return l2m_corrupt(fs, "extent out of range", in->ino);
			*last = (uint64_t)e->ee_block + ext_len(e);
			*nblocks += ext_len(e);
		} else {
			struct ext4_extent_idx *x = idx_at(h, i);
			uint64_t nhi = i + 1 < h->eh_entries ? idx_at(h, i + 1)->ei_block : hi;
			struct epath p = {0};
			int r = read_node(fs, in, idx_pblk(x), depth - 1, &p);
			if (r)
				return r;
			(*nblocks)++;
			if (p.hdr->eh_entries == 0) {
				buf_put(fs, p.buf);
				return l2m_corrupt(fs, "empty extent node", in->ino);
			}
			uint32_t first = depth - 1 == 0 ? ext_at(p.hdr, 0)->ee_block : idx_at(p.hdr, 0)->ei_block;
			if (first != x->ei_block) {
				buf_put(fs, p.buf);
				return l2m_corrupt(fs, "index start does not match child", in->ino);
			}
			r = verify_node(fs, in, p.hdr, depth - 1, x->ei_block, nhi, nblocks, last);
			buf_put(fs, p.buf);
			if (r)
				return r;
		}
	}
	return 0;
}

int ext_verify_tree(l2m_fs *fs, struct l2m_inode *in, uint64_t *nblocks)
{
	struct ext4_extent_header *h = root_hdr(in);
	uint64_t last = 0;
	*nblocks = 0;
	int r = check_hdr(fs, h, true, -1, in->ino);
	if (r)
		return r;
	return verify_node(fs, in, h, h->eh_depth, 0, 0x100000000ull, nblocks, &last);
}

static int walk_node(l2m_fs *fs, struct l2m_inode *in, struct ext4_extent_header *h, int depth, ext_walk_fn fn, void *ctx)
{
	for (int i = 0; i < h->eh_entries; i++) {
		if (depth == 0) {
			struct ext4_extent *e = ext_at(h, i);
			int r = fn(ctx, ext_pblk(e), ext_len(e), false);
			if (r)
				return r;
			continue;
		}
		struct ext4_extent_idx *x = idx_at(h, i);
		int r = fn(ctx, idx_pblk(x), 1, true);
		if (r)
			return r;
		struct epath p = {0};
		r = read_node(fs, in, idx_pblk(x), depth - 1, &p);
		if (r)
			return r;
		r = walk_node(fs, in, p.hdr, depth - 1, fn, ctx);
		buf_put(fs, p.buf);
		if (r)
			return r;
	}
	return 0;
}

int ext_walk(l2m_fs *fs, struct l2m_inode *in, ext_walk_fn fn, void *ctx)
{
	struct ext4_extent_header *h = root_hdr(in);
	int r = check_hdr(fs, h, true, -1, in->ino);
	if (r)
		return r;
	return walk_node(fs, in, h, h->eh_depth, fn, ctx);
}

/* Report lines for a damaged tree: the root, every index entry of the
 * upper levels, and full details of each node whose entries do not fit the
 * range its parent gives it. No checks stop the walk. */
struct descr {
	void (*out)(void *ctx, const char *line);
	void *ctx;
	int lines;
};

static void dline(struct descr *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void dline(struct descr *d, const char *fmt, ...)
{
	if (d->lines++ >= 60)
		return;
	char line[220];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	d->out(d->ctx, line);
}

static void describe_node(l2m_fs *fs, struct ext4_extent_header *h, int depth, uint64_t lo, uint64_t hi, int indent,
			  struct descr *d)
{
	uint32_t maxe = indent == 2 ? ROOT_MAX : node_max(fs);
	if (h->eh_magic != EXT4_EXT_MAGIC || h->eh_entries > maxe || h->eh_depth != depth) {
		dline(d, "%*sBAD HEADER: magic %04x, depth %u (expected %d), %u entries", indent, "", h->eh_magic, h->eh_depth,
		      depth, h->eh_entries);
		return;
	}
	uint64_t last = lo;
	bool ok = true;
	int nbad = 0;
	uint8_t bad[ (4096 - 12) / 12 + 1 ] = {0};
	for (int i = 0; i < h->eh_entries; i++) {
		uint64_t start = depth == 0 ? ext_at(h, i)->ee_block : idx_at(h, i)->ei_block;
		uint64_t end = depth == 0 ? start + ext_len(ext_at(h, i)) : start + 1;
		if (start < last || end > hi || (depth == 0 && ext_len(ext_at(h, i)) == 0)) {
			ok = false;
			nbad++;
			if (i < (int)sizeof(bad))
				bad[i] = 1;
		}
		last = depth == 0 ? end : start;
	}
	if (depth == 0) {
		if (ok)
			return;
		dline(d, "%*sLEAF ENTRIES DO NOT FIT file blocks %llu-%llu (%u entries, %d wrong); wrong ones with neighbours:", indent,
		      "", (unsigned long long)lo, (unsigned long long)hi - 1, h->eh_entries, nbad);
		for (int i = 0; i < h->eh_entries; i++) {
			bool near = false;
			for (int k = L2M_MAX(i - 1, 0); k <= i + 1 && k < (int)sizeof(bad); k++)
				near |= bad[k];
			if (!near && i != h->eh_entries - 1)
				continue;
			struct ext4_extent *e = ext_at(h, i);
			dline(d, "%*s  extent %d: file block %u, %u blocks at %llu%s", indent, "", i, e->ee_block, ext_len(e),
			      (unsigned long long)ext_pblk(e), ext_unwritten(e) ? " (unwritten)" : "");
		}
		return;
	}
	if (!ok)
		dline(d, "%*sINDEX ENTRIES OUT OF ORDER for file blocks %llu-%llu", indent, "", (unsigned long long)lo,
		      (unsigned long long)hi - 1);
	for (int i = 0; i < h->eh_entries; i++) {
		struct ext4_extent_idx *x = idx_at(h, i);
		uint64_t to = i + 1 < h->eh_entries ? idx_at(h, i + 1)->ei_block : hi;
		struct l2m_buf *b;
		if (idx_pblk(x) < fs->first_data_block || idx_pblk(x) >= fs->blocks_count || buf_get(fs, idx_pblk(x), &b)) {
			dline(d, "%*sindex %d: file blocks %u-%llu -> node %llu UNREADABLE", indent, "", i, x->ei_block,
			      (unsigned long long)to - 1, (unsigned long long)idx_pblk(x));
			continue;
		}
		struct ext4_extent_header *c = (struct ext4_extent_header *)b->data;
		dline(d, "%*sindex %d: file blocks %u-%llu -> node %llu (depth %u, %u entries)", indent, "", i, x->ei_block,
		      (unsigned long long)to - 1, (unsigned long long)idx_pblk(x), c->eh_depth, c->eh_entries);
		describe_node(fs, c, depth - 1, x->ei_block, to, indent + 2, d);
		buf_put(fs, b);
	}
}

void ext_describe(l2m_fs *fs, struct l2m_inode *in, void (*out)(void *ctx, const char *line), void *ctx)
{
	struct descr d = {out, ctx, 0};
	struct ext4_extent_header *h = root_hdr(in);
	dline(&d, "  tree of inode %u: depth %u, %u root entries, size %llu bytes", in->ino, h->eh_depth, h->eh_entries,
	      (unsigned long long)inode_size(in));
	describe_node(fs, h, h->eh_depth, 0, 0x100000000ull, 2, &d);
}
