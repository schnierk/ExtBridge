/*
 * ExtBridge - metadata block cache and device helpers.
 *
 * The cache only holds metadata (bitmaps, inode tables, directory and
 * extent blocks, ...). File data never goes through it. Dirty buffers stay
 * referenced by the transaction until it has been checkpointed, clean and
 * unreferenced buffers are kept in an LRU list and evicted when the cache
 * grows above clean_max.
 *
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

#define CACHE_HASH_SIZE 65536u
#define CACHE_CLEAN_MAX 32768u /* 128 MiB with 4 KiB blocks */

int dev_read_blocks(l2m_fs *fs, uint64_t blk, uint32_t count, void *buf)
{
	uint64_t off = blk * fs->bs, len = (uint64_t)count * fs->bs;
	if (blk >= fs->blocks_count || count > fs->blocks_count - blk)
		return l2m_corrupt(fs, "read beyond end of file system", blk);
	if (off + len > fs->dev.size)
		return l2m_corrupt(fs, "read beyond end of device", blk);
	int r = fs->dev.read(fs->dev.ctx, off, buf, (size_t)len);
	if (r < 0) {
		l2m_log(fs, L2M_LOG_ERROR, "device read error %d at block %llu (+%u)", r, (unsigned long long)blk, count);
		return r;
	}
	fs->bytes_read += len;
	return 0;
}

int dev_write_blocks(l2m_fs *fs, uint64_t blk, uint32_t count, const void *buf)
{
	uint64_t off = blk * fs->bs, len = (uint64_t)count * fs->bs;
	if (fs->rdonly || (fs->flags & L2M_MOUNT_RDONLY)) {
		/* This must never happen; it would be a bug in the core. */
		l2m_log(fs, L2M_LOG_ERROR, "BUG: write attempt on read-only volume (block %llu)", (unsigned long long)blk);
		return -EROFS;
	}
	if (blk == 0 && fs->first_data_block == 0 && fs->bs > 1024) {
		/* block 0 holds the boot sector area + superblock; fine */
	}
	if (blk >= fs->blocks_count || count > fs->blocks_count - blk || off + len > fs->dev.size) {
		l2m_log(fs, L2M_LOG_ERROR, "BUG: write beyond end (block %llu)", (unsigned long long)blk);
		return -EIO;
	}
	int r = fs->dev.write(fs->dev.ctx, off, buf, (size_t)len);
	if (r < 0) {
		l2m_log(fs, L2M_LOG_ERROR, "device write error %d at block %llu (+%u)", r, (unsigned long long)blk, count);
		return r;
	}
	fs->bytes_written += len;
	return 0;
}

static int check_range(l2m_fs *fs, uint64_t blk, uint32_t count)
{
	if (blk < fs->first_data_block || blk >= fs->blocks_count || count > fs->blocks_count - blk ||
	    (blk + count) * (uint64_t)fs->bs > fs->dev.size)
		return l2m_corrupt(fs, "data block out of range", blk);
	return 0;
}

int dev_read_data(l2m_fs *fs, uint64_t blk, uint32_t count, void *buf)
{
	if (!fs->dev.read_data)
		return dev_read_blocks(fs, blk, count, buf);
	int r = check_range(fs, blk, count);
	if (r)
		return r;
	r = fs->dev.read_data(fs->dev.ctx, blk * fs->bs, buf, (size_t)count * fs->bs);
	if (!r)
		fs->bytes_read += (uint64_t)count * fs->bs;
	return r;
}

int dev_write_data(l2m_fs *fs, uint64_t blk, uint32_t count, const void *buf)
{
	if (!fs->dev.write_data)
		return dev_write_blocks(fs, blk, count, buf);
	if (fs->rdonly)
		return -EROFS;
	int r = check_range(fs, blk, count);
	if (r)
		return r;
	r = fs->dev.write_data(fs->dev.ctx, blk * fs->bs, buf, (size_t)count * fs->bs);
	if (!r)
		fs->bytes_written += (uint64_t)count * fs->bs;
	return r;
}

int dev_flush(l2m_fs *fs)
{
	if (fs->rdonly)
		return 0;
	return fs->dev.flush ? fs->dev.flush(fs->dev.ctx) : 0;
}

int cache_init(l2m_fs *fs)
{
	struct l2m_cache *c = &fs->cache;
	memset(c, 0, sizeof(*c));
	c->hash_size = CACHE_HASH_SIZE;
	c->clean_max = CACHE_CLEAN_MAX;
	if (fs->bs > 4096)
		c->clean_max = CACHE_CLEAN_MAX / (fs->bs / 4096);
	c->hash = calloc(c->hash_size, sizeof(*c->hash));
	return c->hash ? 0 : -ENOMEM;
}

static inline uint32_t hslot(struct l2m_cache *c, uint64_t blk)
{
	uint64_t h = blk * 0x9E3779B97F4A7C15ull;
	return (uint32_t)(h >> 40) & (c->hash_size - 1);
}

static void lru_remove(struct l2m_cache *c, struct l2m_buf *b)
{
	if (!b->in_lru)
		return;
	if (b->lprev)
		b->lprev->lnext = b->lnext;
	else
		c->lru_head = b->lnext;
	if (b->lnext)
		b->lnext->lprev = b->lprev;
	else
		c->lru_tail = b->lprev;
	b->lprev = b->lnext = NULL;
	b->in_lru = 0;
}

static void lru_push(struct l2m_cache *c, struct l2m_buf *b)
{
	b->lprev = NULL;
	b->lnext = c->lru_head;
	if (c->lru_head)
		c->lru_head->lprev = b;
	c->lru_head = b;
	if (!c->lru_tail)
		c->lru_tail = b;
	b->in_lru = 1;
}

static void hash_remove(struct l2m_cache *c, struct l2m_buf *b)
{
	struct l2m_buf **pp = &c->hash[hslot(c, b->blk)];
	while (*pp && *pp != b)
		pp = &(*pp)->hnext;
	if (*pp)
		*pp = b->hnext;
	b->hnext = NULL;
}

static void buf_free(struct l2m_cache *c, struct l2m_buf *b)
{
	lru_remove(c, b);
	hash_remove(c, b);
	free(b->data);
	free(b);
	c->count--;
}

static struct l2m_buf *lookup(struct l2m_cache *c, uint64_t blk)
{
	for (struct l2m_buf *b = c->hash[hslot(c, blk)]; b; b = b->hnext)
		if (b->blk == blk)
			return b;
	return NULL;
}

static void evict(struct l2m_cache *c)
{
	while (c->count > c->clean_max && c->lru_tail) {
		struct l2m_buf *b = c->lru_tail;
		buf_free(c, b);
	}
}

static int buf_alloc(l2m_fs *fs, uint64_t blk, struct l2m_buf **out)
{
	struct l2m_cache *c = &fs->cache;
	struct l2m_buf *b = calloc(1, sizeof(*b));
	if (!b)
		return -ENOMEM;
	b->data = malloc(fs->bs);
	if (!b->data) {
		free(b);
		return -ENOMEM;
	}
	b->blk = blk;
	uint32_t s = hslot(c, blk);
	b->hnext = c->hash[s];
	c->hash[s] = b;
	c->count++;
	*out = b;
	return 0;
}

int buf_get(l2m_fs *fs, uint64_t blk, struct l2m_buf **out)
{
	struct l2m_cache *c = &fs->cache;
	struct l2m_buf *b = lookup(c, blk);
	if (b) {
		c->hits++;
		lru_remove(c, b);
		b->refs++;
		*out = b;
		return 0;
	}
	c->misses++;
	if (blk >= fs->blocks_count)
		return l2m_corrupt(fs, "metadata block beyond end of file system", blk);
	int r = buf_alloc(fs, blk, &b);
	if (r)
		return r;
	r = dev_read_blocks(fs, blk, 1, b->data);
	if (r) {
		buf_free(c, b);
		return r;
	}
	b->refs = 1;
	*out = b;
	evict(c);
	return 0;
}

int buf_get_new(l2m_fs *fs, uint64_t blk, struct l2m_buf **out)
{
	struct l2m_cache *c = &fs->cache;
	struct l2m_buf *b = lookup(c, blk);
	if (blk >= fs->blocks_count)
		return l2m_corrupt(fs, "new metadata block beyond end of file system", blk);
	if (!b) {
		int r = buf_alloc(fs, blk, &b);
		if (r)
			return r;
	} else {
		lru_remove(c, b);
	}
	memset(b->data, 0, fs->bs);
	b->refs++;
	*out = b;
	evict(c);
	return 0;
}

void buf_put(l2m_fs *fs, struct l2m_buf *b)
{
	if (!b)
		return;
	if (b->refs == 0) {
		l2m_log(fs, L2M_LOG_ERROR, "BUG: buf_put on unreferenced block %llu", (unsigned long long)b->blk);
		return;
	}
	if (--b->refs == 0 && !b->dirty && !b->pinned && !b->overlay) {
		lru_push(&fs->cache, b);
		evict(&fs->cache);
	}
}

int buf_dirty(l2m_fs *fs, struct l2m_buf *b)
{
	if (fs->rdonly)
		return -EROFS;
	if (b->overlay)
		return -EROFS;
	if (!fs->txn.start_ms) {
		int r = txn_begin(fs);
		if (r)
			return r;
	}
	if (!b->dirty) {
		b->dirty = 1;
		b->refs++; /* the transaction holds a reference */
		b->dnext = fs->txn.dirty_head;
		fs->txn.dirty_head = b;
		fs->txn.ndirty++;
	}
	return 0;
}

void buf_forget(l2m_fs *fs, uint64_t blk)
{
	struct l2m_cache *c = &fs->cache;
	struct l2m_buf *b = lookup(c, blk);
	if (!b || b->pinned)
		return;
	if (b->dirty) {
		/* unlink from the transaction list */
		struct l2m_buf **pp = &fs->txn.dirty_head;
		while (*pp && *pp != b)
			pp = &(*pp)->dnext;
		if (*pp)
			*pp = b->dnext;
		b->dnext = NULL;
		b->dirty = 0;
		fs->txn.ndirty--;
		b->refs--;
	}
	if (b->refs == 0)
		buf_free(c, b);
	else
		/* still referenced by a caller: detach from the hash so nobody
		 * finds stale content; the holder frees it on put */
		hash_remove(c, b), b->blk = UINT64_MAX;
}

int cache_peek(l2m_fs *fs, uint64_t blk, struct l2m_buf **out)
{
	struct l2m_buf *b = lookup(&fs->cache, blk);
	if (!b)
		return -ENOENT;
	*out = b;
	return 0;
}

void cache_invalidate_clean(l2m_fs *fs)
{
	struct l2m_cache *c = &fs->cache;
	while (c->lru_tail)
		buf_free(c, c->lru_tail);
}

void cache_destroy(l2m_fs *fs)
{
	struct l2m_cache *c = &fs->cache;
	if (!c->hash)
		return;
	for (uint32_t i = 0; i < c->hash_size; i++) {
		struct l2m_buf *b = c->hash[i];
		while (b) {
			struct l2m_buf *n = b->hnext;
			free(b->data);
			free(b);
			b = n;
		}
	}
	free(c->hash);
	c->hash = NULL;
}
