/*
 * ExtBridge - jbd2 journal: recovery (replay) and transaction commit.
 *
 * Recovery follows the three pass algorithm of jbd2 (scan, revoke,
 * replay) including checksum v2/v3 verification. Replay either writes the
 * blocks to the device (read-write mount) or keeps them as read-only
 * overlay buffers in the cache, so a volume that was not cleanly unmounted
 * can be read correctly without writing anything to it.
 *
 * Commit writes one transaction to the start of the log, makes it durable,
 * checkpoints it and marks the journal empty again:
 *   descriptors+copies -> flush -> commit block + journal sb(s_start)
 *   -> flush -> home locations -> flush -> journal sb(s_start = 0)
 * This keeps the on-disk format 100% compatible with Linux: if we crash,
 * Linux (or e2fsck) replays the committed transaction.
 *
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

#include <time.h>

void sb_finalize(l2m_fs *fs);

/* ---- helpers ------------------------------------------------------------ */

static uint64_t jmap(l2m_fs *fs, uint32_t jblk)
{
	struct l2m_journal *j = &fs->journal;
	uint32_t lo = 0, hi = j->nmap;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		struct l2m_jmap *m = &j->map[mid];
		if (jblk < m->lblk)
			hi = mid;
		else if (jblk >= m->lblk + m->len)
			lo = mid + 1;
		else
			return m->pblk + (jblk - m->lblk);
	}
	return 0;
}

static uint32_t jsb_csum(const struct jbd2_superblock *jsb)
{
	struct jbd2_superblock tmp = *jsb;
	tmp.s_checksum = 0;
	return l2m_crc32c(~0u, &tmp, sizeof(tmp));
}

static uint32_t usable_end(struct l2m_journal *j)
{
	/* last usable log block + 1 (fast commit area excluded) */
	const struct jbd2_superblock *jsb = (const void *)j->sbbuf;
	if (j->incompat & JBD2_FEATURE_INCOMPAT_FAST_COMMIT) {
		uint32_t fc = be32(jsb->s_num_fc_blks);
		if (fc == 0)
			fc = 256;
		if (fc < j->maxlen)
			return j->maxlen - fc;
	}
	return j->maxlen;
}

static uint32_t tag_bytes(struct l2m_journal *j)
{
	if (j->csum_v3)
		return sizeof(struct jbd2_block_tag3);
	uint32_t sz = sizeof(struct jbd2_block_tag);
	if (j->csum_v2)
		sz += sizeof(uint16_t);
	return j->is64 ? sz : sz - sizeof(uint32_t);
}

static void set_features_from(struct l2m_journal *j)
{
	j->csum_v2 = (j->incompat & JBD2_FEATURE_INCOMPAT_CSUM_V2) != 0;
	j->csum_v3 = (j->incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3) != 0;
	j->is64 = (j->incompat & JBD2_FEATURE_INCOMPAT_64BIT) != 0;
	j->tag_bytes = tag_bytes(j);
}

static int jread(l2m_fs *fs, uint32_t jblk, void *buf)
{
	uint64_t p = jmap(fs, jblk);
	if (!p)
		return l2m_corrupt(fs, "journal block not mapped", jblk);
	return dev_read_blocks(fs, p, 1, buf);
}

static int jwrite(l2m_fs *fs, uint32_t jblk, const void *buf)
{
	uint64_t p = jmap(fs, jblk);
	if (!p)
		return l2m_corrupt(fs, "journal block not mapped", jblk);
	return dev_write_blocks(fs, p, 1, buf);
}

static int write_jsb(l2m_fs *fs)
{
	struct l2m_journal *j = &fs->journal;
	struct jbd2_superblock *jsb = (void *)j->sbbuf;
	if (j->csum_v2 || j->csum_v3)
		jsb->s_checksum = be32(jsb_csum(jsb));
	/* the journal superblock occupies the first 1024 bytes of block 0 */
	return jwrite(fs, 0, j->sbbuf);
}

/* ---- load ---------------------------------------------------------------- */

void journal_free(l2m_fs *fs)
{
	free(fs->journal.map);
	free(fs->journal.sbbuf);
	fs->journal.map = NULL;
	fs->journal.sbbuf = NULL;
}

int journal_load(l2m_fs *fs)
{
	struct l2m_journal *j = &fs->journal;
	struct l2m_inode in;
	memset(j, 0, sizeof(*j));
	if (fs->sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_JOURNAL_DEV)
		return -EOPNOTSUPP;
	uint32_t jino = fs->sb->s_journal_inum ? fs->sb->s_journal_inum : EXT4_JOURNAL_INO;
	if (fs->sb->s_journal_dev || !uuid_is_null_bytes(fs->sb->s_journal_uuid)) {
		l2m_log(fs, L2M_LOG_ERROR, "external journal device is not supported");
		return -EOPNOTSUPP;
	}
	int r = inode_read(fs, jino, &in);
	if (r)
		return r;
	uint64_t size = inode_size(&in);
	uint64_t nblocks = size / fs->bs;
	if (nblocks < 1024 || nblocks > 0x7fffffff)
		return l2m_corrupt(fs, "journal inode has an invalid size", nblocks);
	/* build the logical -> physical map of the journal file */
	uint32_t cap = 16;
	j->map = malloc(cap * sizeof(*j->map));
	if (!j->map)
		return -ENOMEM;
	for (uint64_t lb = 0; lb < nblocks;) {
		struct l2m_map m;
		r = bmap(fs, &in, lb, &m);
		if (r)
			return r;
		if (!m.pblk || m.unwritten)
			return l2m_corrupt(fs, "journal file has a hole", lb);
		uint32_t len = (uint32_t)L2M_MIN((uint64_t)m.len, nblocks - lb);
		if (j->nmap && j->map[j->nmap - 1].pblk + j->map[j->nmap - 1].len == m.pblk &&
		    j->map[j->nmap - 1].lblk + j->map[j->nmap - 1].len == lb) {
			j->map[j->nmap - 1].len += len;
		} else {
			if (j->nmap == cap) {
				cap *= 2;
				void *n = realloc(j->map, cap * sizeof(*j->map));
				if (!n)
					return -ENOMEM;
				j->map = n;
			}
			j->map[j->nmap++] = (struct l2m_jmap){lb, m.pblk, len};
		}
		lb += len;
	}
	j->sbbuf = malloc(fs->bs);
	if (!j->sbbuf)
		return -ENOMEM;
	r = jread(fs, 0, j->sbbuf);
	if (r)
		return r;
	struct jbd2_superblock *jsb = (void *)j->sbbuf;
	if (be32(jsb->s_header.h_magic) != JBD2_MAGIC_NUMBER)
		return l2m_corrupt(fs, "journal superblock has a bad magic", 0);
	uint32_t type = be32(jsb->s_header.h_blocktype);
	if (type != JBD2_SUPERBLOCK_V1 && type != JBD2_SUPERBLOCK_V2)
		return l2m_corrupt(fs, "unknown journal superblock type", type);
	j->bs = be32(jsb->s_blocksize);
	j->maxlen = be32(jsb->s_maxlen);
	j->first = be32(jsb->s_first);
	j->sequence = be32(jsb->s_sequence);
	if (j->bs != fs->bs)
		return l2m_corrupt(fs, "journal block size differs from file system", j->bs);
	if (j->maxlen > nblocks || j->first == 0 || j->first >= j->maxlen)
		return l2m_corrupt(fs, "journal geometry invalid", j->maxlen);
	if (type == JBD2_SUPERBLOCK_V2) {
		j->incompat = be32(jsb->s_feature_incompat);
		j->compat = be32(jsb->s_feature_compat);
		uint32_t known = JBD2_FEATURE_INCOMPAT_REVOKE | JBD2_FEATURE_INCOMPAT_64BIT | JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT |
				 JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3 | JBD2_FEATURE_INCOMPAT_FAST_COMMIT;
		if (j->incompat & ~known) {
			l2m_log(fs, L2M_LOG_ERROR, "journal uses unknown features 0x%x", j->incompat & ~known);
			return -EOPNOTSUPP;
		}
		if (be32(jsb->s_nr_users) > 1)
			return -EOPNOTSUPP; /* shared external journal */
	}
	set_features_from(j);
	if (j->csum_v2 || j->csum_v3) {
		if (jsb->s_checksum_type != JBD2_CRC32C_CHKSUM)
			return l2m_corrupt(fs, "journal checksum type unknown", jsb->s_checksum_type);
		if (be32(jsb->s_checksum) != jsb_csum(jsb))
			return l2m_corrupt(fs, "journal superblock checksum wrong", 0);
		j->csum_seed = l2m_crc32c(~0u, jsb->s_uuid, 16);
	}
	if (jsb->s_errno) {
		l2m_log(fs, L2M_LOG_WARN, "journal recorded an error (%d)", (int)be32((uint32_t)jsb->s_errno));
		j->recovery_errors++;
	}
	j->present = 1;
	return 0;
}

/* ---- recovery ----------------------------------------------------------- */

struct revoke_ent {
	uint64_t blk;
	uint32_t seq;
};

struct recovery {
	uint32_t start_seq, end_seq; /* end = first transaction NOT committed */
	struct revoke_ent *rev;
	uint32_t nrev, caprev;
};

static int seq_after(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

static int add_revoke(struct recovery *rc, uint64_t blk, uint32_t seq)
{
	for (uint32_t i = 0; i < rc->nrev; i++)
		if (rc->rev[i].blk == blk) {
			if (seq_after(seq, rc->rev[i].seq))
				rc->rev[i].seq = seq;
			return 0;
		}
	if (rc->nrev == rc->caprev) {
		uint32_t nc = rc->caprev ? rc->caprev * 2 : 64;
		void *n = realloc(rc->rev, nc * sizeof(*rc->rev));
		if (!n)
			return -ENOMEM;
		rc->rev = n;
		rc->caprev = nc;
	}
	rc->rev[rc->nrev++] = (struct revoke_ent){blk, seq};
	return 0;
}

static int is_revoked(struct recovery *rc, uint64_t blk, uint32_t seq)
{
	for (uint32_t i = 0; i < rc->nrev; i++)
		if (rc->rev[i].blk == blk)
			return !seq_after(seq, rc->rev[i].seq); /* revoked at/after seq */
	return 0;
}

static uint32_t wrap(struct l2m_journal *j, uint32_t b)
{
	uint32_t end = usable_end(j);
	return b >= end ? j->first + (b - end) : b;
}

static int desc_csum_ok(struct l2m_journal *j, uint8_t *blk)
{
	if (!j->csum_v2 && !j->csum_v3)
		return 1;
	struct jbd2_block_tail *t = (void *)(blk + j->bs - sizeof(*t));
	uint32_t want = be32(t->t_checksum);
	t->t_checksum = 0;
	uint32_t c = l2m_crc32c(j->csum_seed, blk, j->bs);
	t->t_checksum = be32(want);
	return c == want;
}

static int commit_csum_ok(struct l2m_journal *j, uint8_t *blk)
{
	if (!j->csum_v2 && !j->csum_v3)
		return 1;
	struct jbd2_commit_header *h = (void *)blk;
	uint32_t want = be32(h->h_chksum[0]);
	h->h_chksum[0] = 0;
	uint32_t c = l2m_crc32c(j->csum_seed, blk, j->bs);
	h->h_chksum[0] = be32(want);
	return c == want;
}

static int tag_csum_ok(struct l2m_journal *j, const uint8_t *tag, const uint8_t *data, uint32_t seq)
{
	if (!j->csum_v2 && !j->csum_v3)
		return 1;
	uint32_t s = be32(seq);
	uint32_t c = l2m_crc32c(j->csum_seed, &s, 4);
	c = l2m_crc32c(c, data, j->bs);
	if (j->csum_v3)
		return be32(((const struct jbd2_block_tag3 *)tag)->t_checksum) == c;
	return be16(((const struct jbd2_block_tag *)tag)->t_checksum) == (uint16_t)c;
}

static uint64_t tag_blocknr(struct l2m_journal *j, const uint8_t *tag)
{
	uint64_t b = be32(le32_at(tag));
	if (j->is64)
		b |= (uint64_t)be32(le32_at(tag + 8)) << 32;
	return b;
}

static uint16_t tag_flags(const uint8_t *tag)
{
	/* the low 16 bits of the flags are at byte 6 in all tag formats */
	return be16(le16_at(tag + 6));
}

static int replay_block(l2m_fs *fs, uint64_t blk, uint8_t *data, int to_memory)
{
	if (blk >= fs->blocks_count)
		return l2m_corrupt(fs, "journal replays a block beyond the end", blk);
	if (to_memory) {
		struct l2m_buf *b;
		int r = buf_get_new(fs, blk, &b);
		if (r)
			return r;
		memcpy(b->data, data, fs->bs);
		if (!b->overlay) {
			b->overlay = 1;
			b->refs++; /* overlay buffers stay for the whole mount */
		}
		buf_put(fs, b);
		return 0;
	}
	int r = dev_write_blocks(fs, blk, 1, data);
	if (r)
		return r;
	/* keep cached copies (superblock, descriptors) in sync */
	struct l2m_buf *b;
	if (cache_peek(fs, blk, &b) == 0)
		memcpy(b->data, data, fs->bs);
	return 0;
}

/* pass 0 = scan, 1 = revoke, 2 = replay */
static int do_pass(l2m_fs *fs, struct recovery *rc, int pass, int to_memory)
{
	struct l2m_journal *j = &fs->journal;
	struct jbd2_superblock *jsb = (void *)j->sbbuf;
	uint32_t next_seq = be32(jsb->s_sequence);
	uint32_t blk = be32(jsb->s_start);
	uint8_t *buf = malloc(fs->bs), *data = malloc(fs->bs);
	int r = 0;
	uint32_t visited = 0, limit = usable_end(j) - j->first;
	if (!buf || !data) {
		r = -ENOMEM;
		goto out;
	}
	if (pass == 0)
		rc->start_seq = next_seq;

	for (;;) {
		if (pass != 0 && !seq_after(rc->end_seq, next_seq))
			break;
		if (visited++ > limit) {
			/* went around the whole log without a mismatch */
			if (pass == 0)
				l2m_log(fs, L2M_LOG_WARN, "journal scan wrapped completely");
			break;
		}
		r = jread(fs, blk, buf);
		if (r)
			goto out;
		struct jbd2_header *h = (void *)buf;
		if (be32(h->h_magic) != JBD2_MAGIC_NUMBER || be32(h->h_sequence) != next_seq)
			break;
		uint32_t type = be32(h->h_blocktype);
		uint32_t this_blk = blk;
		blk = wrap(j, blk + 1);

		if (type == JBD2_DESCRIPTOR_BLOCK) {
			if (!desc_csum_ok(j, buf)) {
				if (pass == 0) {
					l2m_log(fs, L2M_LOG_WARN, "journal descriptor %u checksum invalid: end of log", this_blk);
				}
				/* treat like the end of the journal */
				break;
			}
			uint32_t tsize = (j->csum_v2 || j->csum_v3) ? sizeof(struct jbd2_block_tail) : 0;
			uint8_t *tp = buf + sizeof(struct jbd2_header);
			while ((uint32_t)(tp - buf) + j->tag_bytes <= j->bs - tsize) {
				uint16_t fl = tag_flags(tp);
				if (pass == 2) {
					uint64_t target = tag_blocknr(j, tp);
					r = jread(fs, blk, data);
					if (r)
						goto out;
					if (!is_revoked(rc, target, next_seq)) {
						if (!tag_csum_ok(j, tp, data, next_seq)) {
							l2m_log(fs, L2M_LOG_ERROR, "journal: invalid checksum for block %llu (transaction %u)",
								(unsigned long long)target, next_seq);
							j->recovery_errors++;
						} else {
							if (fl & JBD2_FLAG_ESCAPE)
								*(uint32_t *)data = be32(JBD2_MAGIC_NUMBER);
							r = replay_block(fs, target, data, to_memory);
							if (r)
								goto out;
							j->replayed_blocks++;
						}
					}
				}
				blk = wrap(j, blk + 1);
				tp += j->tag_bytes;
				if (!(fl & JBD2_FLAG_SAME_UUID))
					tp += 16;
				if (fl & JBD2_FLAG_LAST_TAG)
					break;
			}
			continue;
		}
		if (type == JBD2_COMMIT_BLOCK) {
			if (pass == 0 && !commit_csum_ok(j, buf)) {
				l2m_log(fs, L2M_LOG_WARN, "journal commit block of transaction %u has a bad checksum: not replayed", next_seq);
				break;
			}
			next_seq++;
			continue;
		}
		if (type == JBD2_REVOKE_BLOCK) {
			if (!desc_csum_ok(j, buf)) {
				if (pass == 0)
					l2m_log(fs, L2M_LOG_WARN, "journal revoke block %u checksum invalid", this_blk);
				break;
			}
			if (pass == 1) {
				struct jbd2_revoke_header *rh = (void *)buf;
				uint32_t cnt = be32(rh->r_count);
				uint32_t rsz = j->is64 ? 8 : 4;
				if (cnt > j->bs)
					cnt = j->bs;
				for (uint32_t off = sizeof(*rh); off + rsz <= cnt; off += rsz) {
					uint64_t b = j->is64 ? be64(*(uint64_t *)(buf + off)) : be32(*(uint32_t *)(buf + off));
					r = add_revoke(rc, b, next_seq);
					if (r)
						goto out;
				}
			}
			continue;
		}
		if (type == JBD2_FC_BLOCK)
			break;
		/* unknown block type: end of log */
		break;
	}
	if (pass == 0)
		rc->end_seq = next_seq;
out:
	free(buf);
	free(data);
	return r;
}

int journal_recover(l2m_fs *fs, int to_memory)
{
	struct l2m_journal *j = &fs->journal;
	struct jbd2_superblock *jsb = (void *)j->sbbuf;
	struct recovery rc = {0};
	int r = 0;
	j->needed_recovery = 1;
	if (jsb->s_start != 0) {
		r = do_pass(fs, &rc, 0, to_memory);
		if (!r)
			r = do_pass(fs, &rc, 1, to_memory);
		if (!r)
			r = do_pass(fs, &rc, 2, to_memory);
		j->replayed_trans = rc.end_seq - rc.start_seq;
		l2m_log(fs, L2M_LOG_INFO, "journal: replayed %u transactions (%u blocks)%s", j->replayed_trans,
			j->replayed_blocks, to_memory ? " in memory" : "");
	}
	free(rc.rev);
	if (r)
		return r;
	if (fs->sb->s_feature_compat & EXT4_FEATURE_COMPAT_FAST_COMMIT && (j->incompat & JBD2_FEATURE_INCOMPAT_FAST_COMMIT)) {
		/* We do not replay fast commit blocks. If the area may contain
		 * a pending fast commit, refuse to write. */
		if (jsb->s_start != 0 || jsb->s_head != 0) {
			l2m_log(fs, L2M_LOG_WARN, "journal may contain fast-commit data we cannot replay");
			j->recovery_errors++;
		}
	}
	if (to_memory)
		return 0;
	if (j->recovery_errors)
		return -EIO;
	/* write back: journal empty, next sequence */
	if (jsb->s_start != 0) {
		r = dev_flush(fs);
		if (r)
			return r;
		j->sequence = rc.end_seq;
		jsb->s_sequence = be32(j->sequence);
		jsb->s_start = 0;
		r = write_jsb(fs);
		if (r)
			return r;
	}
	fs->sb->s_feature_incompat &= ~(uint32_t)EXT4_FEATURE_INCOMPAT_RECOVER;
	sb_finalize(fs);
	r = dev_write_blocks(fs, fs->sbbuf->blk, 1, fs->sbbuf->data);
	if (!r)
		r = dev_flush(fs);
	return r;
}

/* ---- transactions --------------------------------------------------------- */

int txn_begin(l2m_fs *fs)
{
	if (fs->rdonly)
		return -EROFS;
	if (!fs->txn.start_ms)
		fs->txn.start_ms = l2m_now_ms();
	if (!fs->txn.start_ms)
		fs->txn.start_ms = 1;
	return 0;
}

uint32_t txn_max_blocks(l2m_fs *fs)
{
	if (!fs->journal.present)
		return 1u << 20;
	struct l2m_journal *j = &fs->journal;
	uint32_t usable = usable_end(j) - j->first;
	/* descriptors + commit + slack: keep it at half the log */
	uint32_t lim = usable / 2;
	if (lim < 64)
		lim = 64;
	return lim;
}

int txn_add_freed(l2m_fs *fs, uint64_t start, uint64_t len)
{
	struct l2m_txn *t = &fs->txn;
	t->freed_blocks += len;
	if (t->nfreed && t->freed[t->nfreed - 1].start + t->freed[t->nfreed - 1].len == start) {
		t->freed[t->nfreed - 1].len += len;
		return 0;
	}
	if (t->nfreed == t->freed_cap) {
		uint32_t nc = t->freed_cap ? t->freed_cap * 2 : 64;
		void *n = realloc(t->freed, nc * sizeof(*t->freed));
		if (!n)
			return -ENOMEM;
		t->freed = n;
		t->freed_cap = nc;
	}
	t->freed[t->nfreed++] = (struct l2m_range){start, len};
	return 0;
}

bool txn_is_freed(l2m_fs *fs, uint64_t blk, uint64_t *next_ok)
{
	struct l2m_txn *t = &fs->txn;
	for (uint32_t i = 0; i < t->nfreed; i++)
		if (blk >= t->freed[i].start && blk < t->freed[i].start + t->freed[i].len) {
			if (next_ok)
				*next_ok = t->freed[i].start + t->freed[i].len;
			return true;
		}
	return false;
}

static int cmp_buf(const void *a, const void *b)
{
	const struct l2m_buf *x = *(struct l2m_buf *const *)a, *y = *(struct l2m_buf *const *)b;
	return x->blk < y->blk ? -1 : x->blk > y->blk;
}

/* writes the buffers to their home locations, coalescing neighbours */
static int checkpoint(l2m_fs *fs, struct l2m_buf **v, uint32_t n)
{
	uint8_t *big = NULL;
	uint32_t bigcap = 0;
	for (uint32_t i = 0; i < n;) {
		uint32_t k = i + 1;
		while (k < n && v[k]->blk == v[k - 1]->blk + 1 && k - i < 256)
			k++;
		int r;
		if (k - i == 1) {
			r = dev_write_blocks(fs, v[i]->blk, 1, v[i]->data);
		} else {
			if (bigcap < k - i) {
				free(big);
				bigcap = 256;
				big = malloc((size_t)bigcap * fs->bs);
				if (!big)
					return -ENOMEM;
			}
			for (uint32_t m = i; m < k; m++)
				memcpy(big + (size_t)(m - i) * fs->bs, v[m]->data, fs->bs);
			r = dev_write_blocks(fs, v[i]->blk, k - i, big);
		}
		if (r) {
			free(big);
			return r;
		}
		i = k;
	}
	free(big);
	return 0;
}

int journal_write_txn(l2m_fs *fs, struct l2m_buf **v, uint32_t n)
{
	struct l2m_journal *j = &fs->journal;
	struct jbd2_superblock *jsb = (void *)j->sbbuf;
	uint32_t T = j->sequence;
	uint32_t tsize = (j->csum_v2 || j->csum_v3) ? sizeof(struct jbd2_block_tail) : 0;
	uint32_t end = usable_end(j);
	uint8_t *desc = malloc(fs->bs), *copy = malloc(fs->bs);
	int r = 0;
	if (!desc || !copy) {
		r = -ENOMEM;
		goto out;
	}

	if (j->sb_unflushed) {
		/* the previous transaction's "journal empty" mark must hit the
		 * disk before we overwrite its log blocks, otherwise a power cut
		 * could leave the old commit pointing at a mix of old and new
		 * blocks */
		r = dev_flush(fs);
		if (r)
			goto out;
		j->sb_unflushed = 0;
	}
	uint32_t pos = j->first;
	uint32_t i = 0;
	while (i < n) {
		/* one descriptor block followed by its data blocks */
		uint32_t desc_pos = pos++;
		memset(desc, 0, fs->bs);
		struct jbd2_header *h = (void *)desc;
		h->h_magic = be32(JBD2_MAGIC_NUMBER);
		h->h_blocktype = be32(JBD2_DESCRIPTOR_BLOCK);
		h->h_sequence = be32(T);
		uint8_t *tp = desc + sizeof(*h);
		int first = 1;
		uint8_t *last_tag = NULL;
		while (i < n) {
			uint32_t need = j->tag_bytes + (first ? 16 : 0);
			if ((uint32_t)(tp - desc) + need > fs->bs - tsize)
				break;
			if (pos >= end) {
				r = -ENOSPC;
				goto out;
			}
			const uint8_t *src = v[i]->data;
			uint16_t fl = first ? 0 : JBD2_FLAG_SAME_UUID;
			memcpy(copy, src, fs->bs);
			if (be32(*(uint32_t *)copy) == JBD2_MAGIC_NUMBER) {
				*(uint32_t *)copy = 0;
				fl |= JBD2_FLAG_ESCAPE;
			}
			uint64_t blk = v[i]->blk;
			memset(tp, 0, j->tag_bytes);
			*(uint32_t *)tp = be32((uint32_t)blk);
			if (j->csum_v3) {
				struct jbd2_block_tag3 *t3 = (void *)tp;
				t3->t_flags = be32(fl);
				t3->t_blocknr_high = j->is64 ? be32((uint32_t)(blk >> 32)) : 0;
			} else {
				struct jbd2_block_tag *t = (void *)tp;
				t->t_flags = be16(fl);
				if (j->is64)
					t->t_blocknr_high = be32((uint32_t)(blk >> 32));
			}
			if (j->csum_v2 || j->csum_v3) {
				uint32_t s = be32(T);
				uint32_t c = l2m_crc32c(j->csum_seed, &s, 4);
				c = l2m_crc32c(c, copy, fs->bs);
				if (j->csum_v3)
					((struct jbd2_block_tag3 *)tp)->t_checksum = be32(c);
				else
					((struct jbd2_block_tag *)tp)->t_checksum = be16((uint16_t)c);
			}
			last_tag = tp;
			tp += j->tag_bytes;
			if (first) {
				memcpy(tp, jsb->s_uuid, 16);
				tp += 16;
				first = 0;
			}
			r = jwrite(fs, pos++, copy);
			if (r)
				goto out;
			i++;
		}
		if (!last_tag) {
			r = -EINVAL;
			goto out;
		}
		/* mark the last tag */
		if (j->csum_v3) {
			struct jbd2_block_tag3 *t3 = (void *)last_tag;
			t3->t_flags = be32(be32(t3->t_flags) | JBD2_FLAG_LAST_TAG);
		} else {
			struct jbd2_block_tag *t = (void *)last_tag;
			t->t_flags = be16((uint16_t)(be16(t->t_flags) | JBD2_FLAG_LAST_TAG));
		}
		if (tsize) {
			struct jbd2_block_tail *tl = (void *)(desc + fs->bs - tsize);
			tl->t_checksum = 0;
			tl->t_checksum = be32(l2m_crc32c(j->csum_seed, desc, fs->bs));
		}
		r = jwrite(fs, desc_pos, desc);
		if (r)
			goto out;
	}
	if (pos >= end) {
		r = -ENOSPC;
		goto out;
	}
	r = dev_flush(fs);
	if (r)
		goto out;

	/* commit block */
	memset(desc, 0, fs->bs);
	struct jbd2_commit_header *ch = (void *)desc;
	ch->h_magic = be32(JBD2_MAGIC_NUMBER);
	ch->h_blocktype = be32(JBD2_COMMIT_BLOCK);
	ch->h_sequence = be32(T);
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ch->h_commit_sec = be64((uint64_t)ts.tv_sec);
	ch->h_commit_nsec = be32((uint32_t)ts.tv_nsec);
	if (j->csum_v2 || j->csum_v3)
		ch->h_chksum[0] = be32(l2m_crc32c(j->csum_seed, desc, fs->bs));
	r = jwrite(fs, pos++, desc);
	if (r)
		goto out;
	jsb->s_start = be32(j->first);
	jsb->s_sequence = be32(T);
	r = write_jsb(fs);
	if (!r)
		r = dev_flush(fs);
	if (r)
		goto out;
	/* transaction is durable now; write home locations */
	r = checkpoint(fs, v, n);
	if (!r)
		r = dev_flush(fs);
	if (r)
		goto out;
	j->sequence = T + 1;
	jsb->s_start = 0;
	jsb->s_sequence = be32(j->sequence);
	r = write_jsb(fs);
	if (!r)
		j->sb_unflushed = 1;
out:
	free(desc);
	free(copy);
	return r;
}

static void prepare_journal_features(l2m_fs *fs)
{
	struct l2m_journal *j = &fs->journal;
	struct jbd2_superblock *jsb = (void *)j->sbbuf;
	if (be32(jsb->s_header.h_blocktype) != JBD2_SUPERBLOCK_V2)
		return;
	uint32_t inc = j->incompat;
	/* like Linux: checksummed fs -> csum v3 journal, 64bit fs -> 64bit tags */
	if (fs->has_csum && !(inc & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3))) {
		inc |= JBD2_FEATURE_INCOMPAT_CSUM_V3;
		jsb->s_checksum_type = JBD2_CRC32C_CHKSUM;
		j->compat &= ~(uint32_t)JBD2_FEATURE_COMPAT_CHECKSUM;
		jsb->s_feature_compat = be32(j->compat);
		j->csum_seed = l2m_crc32c(~0u, jsb->s_uuid, 16);
	}
	if (fs->is64 && !(inc & JBD2_FEATURE_INCOMPAT_64BIT))
		inc |= JBD2_FEATURE_INCOMPAT_64BIT;
	if (inc != j->incompat) {
		j->incompat = inc;
		jsb->s_feature_incompat = be32(inc);
		set_features_from(j);
	}
}

int journal_mark_empty(l2m_fs *fs)
{
	(void)fs;
	return 0;
}

int txn_commit(l2m_fs *fs)
{
	struct l2m_txn *t = &fs->txn;
	int r = 0;
	if (fs->rdonly)
		return 0;
	if (!t->ndirty) {
		t->start_ms = 0;
		t->nfreed = 0;
		t->freed_blocks = 0;
		t->ops = 0;
		return 0;
	}
	if (fs->sbbuf->dirty)
		sb_finalize(fs);
	struct l2m_buf **v = malloc(t->ndirty * sizeof(*v));
	if (!v)
		return -ENOMEM;
	uint32_t n = 0;
	for (struct l2m_buf *b = t->dirty_head; b; b = b->dnext)
		v[n++] = b;
	qsort(v, n, sizeof(*v), cmp_buf);
	if (fs->journal.present) {
		prepare_journal_features(fs);
		r = journal_write_txn(fs, v, n);
	} else {
		r = checkpoint(fs, v, n);
		if (!r)
			r = dev_flush(fs);
	}
	if (r) {
		/* The device refused a write. We cannot know what reached the
		 * disk; switch to read-only so nothing makes it worse. The
		 * journal guarantees consistency on the next mount. */
		l2m_log(fs, L2M_LOG_ERROR, "commit failed (%d): volume switched to read-only", r);
		fs->rdonly = 1;
		fs->ro_reason = L2M_RO_ERRORS;
		free(v);
		return r;
	}
	for (uint32_t i = 0; i < n; i++) {
		v[i]->dirty = 0;
		v[i]->dnext = NULL;
		buf_put(fs, v[i]); /* drop the transaction reference */
	}
	free(v);
	t->dirty_head = NULL;
	t->ndirty = 0;
	t->start_ms = 0;
	t->nfreed = 0;
	t->freed_blocks = 0;
	t->ops = 0;
	return 0;
}

int txn_enospc_retry(l2m_fs *fs)
{
	if (fs->rdonly || fs->txn.freed_blocks == 0)
		return 0;
	int r = txn_commit(fs);
	return r ? r : 1;
}

int txn_end_op(l2m_fs *fs)
{
	struct l2m_txn *t = &fs->txn;
	if (fs->rdonly || !t->start_ms)
		return 0;
	t->ops++;
	/* big deletes are committed right away so the space is reusable at
	 * once (freed blocks are held back until their transaction commits) */
	bool big_free = t->freed_blocks >= 32768 || t->freed_blocks * 16 >= sb_free_blocks(fs);
	if ((fs->flags & L2M_MOUNT_SYNC) || big_free || t->ndirty >= fs->txn_limit / 2 ||
	    l2m_now_ms() - t->start_ms >= fs->commit_interval_ms)
		return txn_commit(fs);
	return 0;
}

int txn_need_room(l2m_fs *fs, uint32_t blocks)
{
	if (fs->txn.ndirty + blocks >= fs->txn_limit)
		return txn_commit(fs);
	return 0;
}

int l2m_sync(l2m_fs *fs)
{
	pthread_mutex_lock(&fs->lock);
	int r = txn_commit(fs);
	if (!r && !fs->rdonly)
		r = dev_flush(fs);
	pthread_mutex_unlock(&fs->lock);
	return r;
}

int l2m_sync_if_older(l2m_fs *fs, unsigned max_age_ms)
{
	int r = 0;
	pthread_mutex_lock(&fs->lock);
	if (fs->txn.start_ms && l2m_now_ms() - fs->txn.start_ms >= max_age_ms)
		r = txn_commit(fs);
	pthread_mutex_unlock(&fs->lock);
	return r;
}
