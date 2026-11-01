/*
 * ExtBridge - extended attributes (in-inode and one external block).
 *
 * Every change rebuilds the attribute set: small attributes go into the
 * inode body, the rest into an external block (shared blocks are copied on
 * write). Values that would need an EA inode (ea_inode feature) are
 * rejected with ENOSPC.
 *
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

struct xa {
	uint8_t index;
	uint8_t name_len;
	char name[256];
	uint32_t size;
	uint8_t *value;
};

struct xaset {
	struct xa *v;
	uint32_t n, cap;
};

static void xaset_free(struct xaset *s)
{
	for (uint32_t i = 0; i < s->n; i++)
		free(s->v[i].value);
	free(s->v);
	memset(s, 0, sizeof(*s));
}

static int xaset_add(struct xaset *s, uint8_t index, const char *name, size_t nlen, const void *val, size_t size)
{
	if (s->n == s->cap) {
		uint32_t nc = s->cap ? s->cap * 2 : 8;
		void *p = realloc(s->v, nc * sizeof(*s->v));
		if (!p)
			return -ENOMEM;
		s->v = p;
		s->cap = nc;
	}
	struct xa *x = &s->v[s->n];
	memset(x, 0, sizeof(*x));
	x->index = index;
	x->name_len = (uint8_t)nlen;
	memcpy(x->name, name, nlen);
	x->size = (uint32_t)size;
	x->value = malloc(size ? size : 1);
	if (!x->value)
		return -ENOMEM;
	memcpy(x->value, val, size);
	s->n++;
	return 0;
}

static uint32_t entry_hash(const struct xa *x)
{
	uint32_t h = 0;
	for (int i = 0; i < x->name_len; i++)
		h = (h << 5) ^ (h >> 27) ^ (unsigned char)x->name[i];
	uint32_t words = (x->size + 3) / 4;
	for (uint32_t w = 0; w < words; w++) {
		uint32_t v = 0;
		uint32_t take = L2M_MIN(4u, x->size - w * 4);
		memcpy(&v, x->value + w * 4, take);
		h = (h << 16) ^ (h >> 16) ^ v;
	}
	return h;
}

static uint32_t block_csum(l2m_fs *fs, uint64_t blk, const uint8_t *b)
{
	uint64_t le = blk;
	uint32_t dummy = 0;
	size_t off = offsetof(struct ext4_xattr_header, h_checksum);
	uint32_t c = l2m_crc32c(fs->csum_seed, &le, 8);
	c = l2m_crc32c(c, b, off);
	c = l2m_crc32c(c, &dummy, 4);
	return l2m_crc32c(c, b + off + 4, fs->bs - off - 4);
}

static uint32_t ibody_start(l2m_fs *fs, struct l2m_inode *in)
{
	(void)fs;
	return EXT4_GOOD_OLD_INODE_SIZE + in->i.i_extra_isize;
}

/* parse entries in [entries, end) with values relative to vbase */
static int parse_entries(l2m_fs *fs, struct l2m_inode *in, const uint8_t *entries, const uint8_t *end, const uint8_t *vbase, size_t vlen, struct xaset *s)
{
	const uint8_t *p = entries;
	while (p + 4 <= end && le32_at(p) != 0) {
		const struct ext4_xattr_entry *e = (const void *)p;
		if (p + sizeof(*e) + e->e_name_len > end)
			return l2m_corrupt(fs, "xattr entry runs past the end", in->ino);
		if (e->e_value_inum) {
			/* value in an EA inode: not supported, skip */
			l2m_log(fs, L2M_LOG_WARN, "inode %u: xattr in EA inode skipped", in->ino);
		} else {
			if ((size_t)e->e_value_offs + e->e_value_size > vlen)
				return l2m_corrupt(fs, "xattr value out of range", in->ino);
			int r = xaset_add(s, e->e_name_index, e->e_name, e->e_name_len, vbase + e->e_value_offs, e->e_value_size);
			if (r)
				return r;
		}
		p += EXT4_XATTR_LEN(e->e_name_len);
	}
	return 0;
}

static int load_all(l2m_fs *fs, struct l2m_inode *in, struct xaset *s, uint32_t *refcount)
{
	memset(s, 0, sizeof(*s));
	*refcount = 0;
	if (fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE && in->i.i_extra_isize) {
		uint32_t st = ibody_start(fs, in);
		if (st + 4 <= fs->inode_size && le32_at(in->raw + st) == EXT4_XATTR_MAGIC) {
			const uint8_t *first = in->raw + st + 4;
			int r = parse_entries(fs, in, first, in->raw + fs->inode_size, first, (size_t)(in->raw + fs->inode_size - first), s);
			if (r) {
				xaset_free(s);
				return r;
			}
		}
	}
	uint64_t blk = inode_file_acl(in);
	if (blk) {
		struct l2m_buf *b;
		if (blk >= fs->blocks_count) {
			xaset_free(s);
			return l2m_corrupt(fs, "xattr block out of range", in->ino);
		}
		int r = buf_get(fs, blk, &b);
		if (r) {
			xaset_free(s);
			return r;
		}
		struct ext4_xattr_header *h = (void *)b->data;
		if (h->h_magic != EXT4_XATTR_MAGIC || h->h_blocks != 1) {
			buf_put(fs, b);
			xaset_free(s);
			return l2m_corrupt(fs, "xattr block header invalid", blk);
		}
		if (fs->has_csum && fs->verify_csums && !b->dirty && h->h_checksum != block_csum(fs, blk, b->data)) {
			buf_put(fs, b);
			xaset_free(s);
			l2m_log(fs, L2M_LOG_ERROR, "xattr block %llu checksum mismatch", (unsigned long long)blk);
			return -EFSBADCRC;
		}
		*refcount = h->h_refcount;
		r = parse_entries(fs, in, b->data + sizeof(*h), b->data + fs->bs, b->data, fs->bs, s);
		buf_put(fs, b);
		if (r) {
			xaset_free(s);
			return r;
		}
	}
	return 0;
}

static int xa_cmp(const void *a, const void *b)
{
	const struct xa *x = a, *y = b;
	if (x->index != y->index)
		return x->index - y->index;
	if (x->name_len != y->name_len)
		return x->name_len - y->name_len;
	return memcmp(x->name, y->name, x->name_len);
}

/* lay out entries into region [base, base+len); entries start at estart
 * (offset from base), values are relative to voff_base. Returns 0 or
 * -ENOSPC. */
static int layout(struct xa **items, uint32_t n, uint8_t *base, uint32_t len, uint32_t estart, uint32_t voff_base, bool hashes)
{
	memset(base + estart, 0, len - estart);
	uint32_t epos = estart, vend = len;
	for (uint32_t i = 0; i < n; i++) {
		struct xa *x = items[i];
		uint32_t el = EXT4_XATTR_LEN(x->name_len), vl = EXT4_XATTR_SIZE(x->size);
		if (vl > vend || epos + el + 4 > vend - vl)
			return -ENOSPC;
		vend -= vl;
		struct ext4_xattr_entry *e = (void *)(base + epos);
		e->e_name_len = x->name_len;
		e->e_name_index = x->index;
		e->e_value_offs = (uint16_t)(x->size ? vend - voff_base : 0);
		e->e_value_inum = 0;
		e->e_value_size = x->size;
		e->e_hash = hashes ? entry_hash(x) : 0;
		memcpy(e->e_name, x->name, x->name_len);
		if (x->size)
			memcpy(base + vend, x->value, x->size);
		epos += el;
	}
	return 0;
}

static int drop_block(l2m_fs *fs, struct l2m_inode *in)
{
	uint64_t blk = inode_file_acl(in);
	if (!blk)
		return 0;
	struct l2m_buf *b;
	int r = buf_get(fs, blk, &b);
	if (r)
		return r;
	struct ext4_xattr_header *h = (void *)b->data;
	if (h->h_refcount > 1) {
		h->h_refcount--;
		if (fs->has_csum)
			h->h_checksum = block_csum(fs, blk, b->data);
		r = buf_dirty(fs, b);
		buf_put(fs, b);
	} else {
		buf_put(fs, b);
		buf_forget(fs, blk);
		r = free_blocks(fs, blk, 1);
	}
	if (r)
		return r;
	inode_add_blocks(fs, in, -1);
	inode_set_file_acl(in, 0);
	return 0;
}

static int store_all(l2m_fs *fs, struct l2m_inode *in, struct xaset *s, uint32_t refcount)
{
	struct xa **items = malloc((s->n + 1) * sizeof(*items));
	if (!items)
		return -ENOMEM;
	for (uint32_t i = 0; i < s->n; i++)
		items[i] = &s->v[i];
	/* in-inode first, smallest entries first (best fit), rest in block */
	uint32_t st = ibody_start(fs, in);
	bool ibody = fs->inode_size > EXT4_GOOD_OLD_INODE_SIZE && in->i.i_extra_isize && st + 8 <= fs->inode_size;
	uint32_t nin = 0;
	int r = 0;
	if (ibody) {
		/* sort by size and take as many as fit */
		for (uint32_t i = 0; i < s->n; i++)
			for (uint32_t k = i + 1; k < s->n; k++) {
				uint32_t si = EXT4_XATTR_LEN(items[i]->name_len) + EXT4_XATTR_SIZE(items[i]->size);
				uint32_t sk = EXT4_XATTR_LEN(items[k]->name_len) + EXT4_XATTR_SIZE(items[k]->size);
				if (sk < si) {
					struct xa *t = items[i];
					items[i] = items[k];
					items[k] = t;
				}
			}
		uint8_t *region = in->raw + st + 4;
		uint32_t rlen = fs->inode_size - st - 4;
		uint32_t used = 4; /* terminator */
		while (nin < s->n) {
			uint32_t need = EXT4_XATTR_LEN(items[nin]->name_len) + EXT4_XATTR_SIZE(items[nin]->size);
			if (used + need > rlen)
				break;
			used += need;
			nin++;
		}
		if (nin) {
			*(uint32_t *)(in->raw + st) = EXT4_XATTR_MAGIC;
			r = layout(items, nin, region, rlen, 0, 0, true);
			if (r)
				goto out;
		} else {
			memset(in->raw + st, 0, fs->inode_size - st);
		}
	}
	uint32_t nblk = s->n - nin;
	if (nblk == 0) {
		r = drop_block(fs, in);
		goto out;
	}
	if (!fs->has_xattr) {
		r = -ENOSPC;
		goto out;
	}
	{
		/* block entries are kept sorted like Linux does */
		for (uint32_t i = nin; i < s->n; i++)
			for (uint32_t k = i + 1; k < s->n; k++)
				if (xa_cmp(items[k], items[i]) < 0) {
					struct xa *t = items[i];
					items[i] = items[k];
					items[k] = t;
				}
	}
	uint8_t *blkbuf = calloc(1, fs->bs);
	if (!blkbuf) {
		r = -ENOMEM;
		goto out;
	}
	struct ext4_xattr_header *h = (void *)blkbuf;
	r = layout(items + nin, nblk, blkbuf, fs->bs, sizeof(*h), 0, true);
	if (r) {
		free(blkbuf);
		goto out;
	}
	h->h_magic = EXT4_XATTR_MAGIC;
	h->h_refcount = 1;
	h->h_blocks = 1;
	uint32_t hh = 0;
	for (struct ext4_xattr_entry *e = (void *)(blkbuf + sizeof(*h)); le32_at(e) != 0;
	     e = (void *)((uint8_t *)e + EXT4_XATTR_LEN(e->e_name_len))) {
		if (!e->e_hash) {
			hh = 0;
			break;
		}
		hh = (hh << 16) ^ (hh >> 16) ^ e->e_hash;
	}
	h->h_hash = hh;
	uint64_t blk = inode_file_acl(in);
	if (blk && refcount > 1) {
		r = drop_block(fs, in); /* shared: decrement, then make our own */
		blk = 0;
		if (r) {
			free(blkbuf);
			goto out;
		}
	}
	if (!blk) {
		uint32_t got;
		r = alloc_blocks(fs, goal_for_inode(fs, in->ino), 1, &blk, &got, true);
		if (r) {
			free(blkbuf);
			goto out;
		}
		inode_add_blocks(fs, in, 1);
		inode_set_file_acl(in, blk);
	}
	if (fs->has_csum)
		h->h_checksum = block_csum(fs, blk, blkbuf);
	struct l2m_buf *b;
	r = buf_get_new(fs, blk, &b);
	if (!r) {
		memcpy(b->data, blkbuf, fs->bs);
		r = buf_dirty(fs, b);
		buf_put(fs, b);
	}
	free(blkbuf);
	if (!r && !(fs->sb->s_feature_compat & EXT4_FEATURE_COMPAT_EXT_ATTR)) {
		fs->sb->s_feature_compat |= EXT4_FEATURE_COMPAT_EXT_ATTR;
		r = sb_dirty(fs);
	}
out:
	free(items);
	return r;
}

int xattr_get(l2m_fs *fs, struct l2m_inode *in, int index, const char *name, void *buf, size_t len)
{
	struct xaset s;
	uint32_t rc;
	int r = load_all(fs, in, &s, &rc);
	if (r)
		return r;
	size_t nl = strlen(name);
	r = -ENODATA;
	for (uint32_t i = 0; i < s.n; i++) {
		if (s.v[i].index == index && s.v[i].name_len == nl && memcmp(s.v[i].name, name, nl) == 0) {
			if (buf && len) {
				if (len < s.v[i].size)
					r = -ERANGE;
				else {
					memcpy(buf, s.v[i].value, s.v[i].size);
					r = (int)s.v[i].size;
				}
			} else {
				r = (int)s.v[i].size;
			}
			break;
		}
	}
	xaset_free(&s);
	return r;
}

int xattr_set(l2m_fs *fs, struct l2m_inode *in, int index, const char *name, const void *val, size_t len, unsigned flags)
{
	struct xaset s;
	uint32_t rc;
	size_t nl = strlen(name);
	if (nl == 0 || nl > 255)
		return -ERANGE;
	if (val && len > fs->bs - sizeof(struct ext4_xattr_header) - EXT4_XATTR_LEN(nl) - 4)
		return -ENOSPC;
	int r = load_all(fs, in, &s, &rc);
	if (r)
		return r;
	int found = -1;
	for (uint32_t i = 0; i < s.n; i++)
		if (s.v[i].index == index && s.v[i].name_len == nl && memcmp(s.v[i].name, name, nl) == 0)
			found = (int)i;
	if (!val) {
		if (found < 0) {
			xaset_free(&s);
			return -ENODATA;
		}
		free(s.v[found].value);
		s.v[found] = s.v[--s.n];
	} else {
		if (found >= 0 && (flags & L2M_XATTR_CREATE)) {
			xaset_free(&s);
			return -EEXIST;
		}
		if (found < 0 && (flags & L2M_XATTR_REPLACE)) {
			xaset_free(&s);
			return -ENODATA;
		}
		if (found >= 0) {
			void *nv = malloc(len ? len : 1);
			if (!nv) {
				xaset_free(&s);
				return -ENOMEM;
			}
			memcpy(nv, val, len);
			free(s.v[found].value);
			s.v[found].value = nv;
			s.v[found].size = (uint32_t)len;
		} else {
			r = xaset_add(&s, (uint8_t)index, name, nl, val, len);
			if (r) {
				xaset_free(&s);
				return r;
			}
		}
	}
	r = store_all(fs, in, &s, rc);
	xaset_free(&s);
	return r;
}

static const char *prefix_of(int index)
{
	switch (index) {
	case EXT4_XATTR_INDEX_USER: return "";
	case EXT4_XATTR_INDEX_POSIX_ACL_ACCESS: return "system.posix_acl_access";
	case EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT: return "system.posix_acl_default";
	case EXT4_XATTR_INDEX_TRUSTED: return "trusted.";
	case EXT4_XATTR_INDEX_SECURITY: return "security.";
	case EXT4_XATTR_INDEX_SYSTEM: return "system.";
	case EXT4_XATTR_INDEX_RICHACL: return "system.richacl";
	}
	return NULL;
}

int64_t xattr_list(l2m_fs *fs, struct l2m_inode *in, char *buf, size_t len)
{
	struct xaset s;
	uint32_t rc;
	int r = load_all(fs, in, &s, &rc);
	if (r)
		return r;
	size_t total = 0;
	for (uint32_t i = 0; i < s.n; i++) {
		/* only the user namespace is exposed to macOS */
		if (s.v[i].index != EXT4_XATTR_INDEX_USER)
			continue;
		const char *p = prefix_of(s.v[i].index);
		size_t need = strlen(p) + s.v[i].name_len + 1;
		if (buf && len) {
			if (total + need > len) {
				xaset_free(&s);
				return -ERANGE;
			}
			memcpy(buf + total, p, strlen(p));
			memcpy(buf + total + strlen(p), s.v[i].name, s.v[i].name_len);
			buf[total + need - 1] = 0;
		}
		total += need;
	}
	xaset_free(&s);
	return (int64_t)total;
}

int xattr_free_all(l2m_fs *fs, struct l2m_inode *in)
{
	return drop_block(fs, in);
}
