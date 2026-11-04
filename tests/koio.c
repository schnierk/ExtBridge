/*
 * ExtBridge - big downloads written the way macOS writes them through
 * FSKit's kernel-offloaded I/O, including the imperfect reports seen on a
 * real Mac.
 *
 *   koio IMAGE MODE [seed]
 *
 * A simulated kernel keeps a page cache (16 KiB pages) for several files
 * that grow by small appends at the same time, like browser downloads, and
 * writes dirty pages out in numbered operations: write-behind of full 64 MiB
 * windows, syncs of everything up to the end of the file, 1 MiB pageouts
 * and a final flush. Each operation maps what it has not mapped before
 * (l2m_koio_map, at most 16 extents per call like FSKit's packer), writes
 * the data straight onto the device and reports one summary range
 * (l2m_koio_done); writes through mappings it kept report operation 0.
 *
 *   download    the protocol as observed on macOS 26
 *   misreport   some reports leave out the beginning or the end of what the
 *               operation wrote (zero holes before 0.9.3)
 *   lost        some reports never come, some operations fail and are
 *               written again later
 *   stale       a preallocated file with old data on the disk is written
 *               in two places by one operation whose report spans the gap:
 *               the gap must still read as zeros
 *   merge       one file appended in many small operations: few extents
 *
 * Afterwards every file must read back exactly, contain no zero holes and
 * l2m_verify must find nothing; tests/run_all.sh runs e2fsck on the image.
 *
 * SPDX-License-Identifier: MIT
 */
#define _FILE_OFFSET_BITS 64
#include "l2m/l2m.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAGE 16384ull
#define WINDOW (64ull << 20)
#define PACKER 16

static l2m_fs *fs;
static int dev_fd;
static uint64_t bs;
static uint64_t opseq = 1000;
static const char *mode;

#define CHECK(what, expr)                                                                                   \
	do {                                                                                                \
		int64_t r_ = (expr);                                                                        \
		if (r_ < 0) {                                                                               \
			fprintf(stderr, "koio: %s failed: %lld (%s)\n", what, (long long)r_, strerror((int)-r_)); \
			exit(1);                                                                            \
		}                                                                                           \
	} while (0)

static unsigned seed = 1;
static unsigned rnd(void)
{
	seed = seed * 1103515245u + 12345u;
	return seed >> 8;
}
static bool chance(unsigned percent) { return rnd() % 100 < percent; }

static uint8_t pattern(uint64_t id, uint64_t off) { return (uint8_t)((off * 2654435761u) >> 13 ^ id * 151 ^ off >> 17); }

struct sfile {
	uint32_t ino;
	uint64_t id, size, target;
	uint8_t *dirty;   /* per page: 1 dirty, 2 dirty again after a failed write */
	uint64_t *pblk;   /* per block: kept mapping, 0 = none */
	uint64_t next_sync;
};

static bool closing; /* the final flush: reports arrive (the file size depends on them) */

/* one operation: map what is not mapped yet, write the dirty pages of
 * [lo, hi), report */
static void push(struct sfile *f, uint64_t lo, uint64_t hi)
{
	uint64_t op = ++opseq, first = ~0ull, last = 0, maps = 0, last_map = 0;
	uint64_t eofb = (f->size + bs - 1) / bs;
	static uint8_t buf[PAGE];
	bool retry = false;
	for (uint64_t pg = lo; pg < hi; pg++) {
		if (f->dirty[pg] == 2)
			retry = true;
		if (!f->dirty[pg])
			continue;
		uint64_t b0 = pg * PAGE / bs, b1 = (pg + 1) * PAGE / bs;
		if (b1 > eofb)
			b1 = eofb;
		/* map the blocks of this page that have no kept mapping */
		for (uint64_t b = b0; b < b1;) {
			if (f->pblk[b]) {
				b++;
				continue;
			}
			uint64_t e = b;
			while (e < b1 && !f->pblk[e])
				e++;
			while (b < e) {
				l2m_extent ex[64];
				last_map = b * bs;
				int n = l2m_koio_map(fs, f->ino, op, b, e - b, L2M_MAP_ALLOCATE, ex, 64);
				CHECK("map", n);
				if (n > PACKER) {
					/* FSKit's packer takes 16 extents; the rest is asked again */
					CHECK("unmap", l2m_koio_unmap(fs, f->ino, op, ex[PACKER].lblk, e - ex[PACKER].lblk));
					n = PACKER;
				}
				for (int i = 0; i < n; i++) {
					if (ex[i].type == L2M_EXT_HOLE) {
						fprintf(stderr, "koio: hole after allocation at block %llu\n", (unsigned long long)ex[i].lblk);
						exit(1);
					}
					for (uint32_t k = 0; k < ex[i].len; k++)
						f->pblk[ex[i].lblk + k] = ex[i].pblk + k;
					b = ex[i].lblk + ex[i].len;
				}
				maps++;
			}
		}
		/* write the page's blocks (zeros after the end of the file) */
		for (uint64_t b = b0; b < b1; b++) {
			for (uint64_t j = 0; j < bs; j++)
				buf[j] = b * bs + j < f->size ? pattern(f->id, b * bs + j) : 0;
			if (pwrite(dev_fd, buf, bs, (off_t)(f->pblk[b] * bs)) != (ssize_t)bs) {
				perror("pwrite");
				exit(1);
			}
		}
		f->dirty[pg] = 0;
		if (pg * PAGE < first)
			first = pg * PAGE;
		last = (pg + 1) * PAGE;
	}
	if (first == ~0ull)
		return;
	if (last > f->size)
		last = f->size;
	uint64_t rop = maps ? op : 0, from = first, to = last;
	int ok = 1;
	if (!strcmp(mode, "misreport") && maps && chance(30) && last_map > from && last_map < to)
		from = last_map; /* the report starts at the operation's last mapping */
	if (!strcmp(mode, "lost") && !closing && !retry) {
		/* a lost report - but not for the retry of a failed operation
		 * (two faults in a row are not the case this models) */
		if (chance(10))
			return;
		if (chance(5)) {
			/* failed: the pages stay dirty and are written again later */
			ok = 0;
			for (uint64_t pg = first / PAGE; pg * PAGE < last; pg++)
				f->dirty[pg] = 2;
		}
	}
	CHECK("done", l2m_koio_done(fs, f->ino, rop, from, to - from, ok));
}

static void push_all(struct sfile *f) { push(f, 0, (f->size + PAGE - 1) / PAGE); }

/* the application appends n bytes; the kernel reacts like macOS */
static void append(struct sfile *f, uint64_t n)
{
	uint64_t old = f->size;
	f->size += n;
	for (uint64_t pg = old / PAGE; pg * PAGE < f->size; pg++)
		if (!f->dirty[pg])
			f->dirty[pg] = 1;
	if (old / WINDOW != f->size / WINDOW) /* write-behind of the finished window */
		push(f, (old / WINDOW) * WINDOW / PAGE, (old / WINDOW + 1) * WINDOW / PAGE);
	if (f->size >= f->next_sync) { /* a sync: everything up to the end */
		push_all(f);
		f->next_sync = f->size + (rnd() % 64 + 1) * (1ull << 20);
	}
	if (strcmp(mode, "merge") && chance(3)) { /* memory pressure: one 1 MiB cluster goes out */
		uint64_t pg = (rnd() % ((f->size + PAGE - 1) / PAGE)) & ~63ull;
		push(f, pg, pg + 64);
	}
}

static void check_file(struct sfile *f, const char *when, int *bad)
{
	static uint8_t buf[1 << 20];
	for (uint64_t p = 0; p < f->size; p += sizeof(buf)) {
		size_t k = (size_t)(f->size - p < sizeof(buf) ? f->size - p : sizeof(buf));
		int64_t n = l2m_read(fs, f->ino, p, buf, k);
		CHECK("read back", n);
		for (size_t j = 0; j < k; j++)
			if (buf[j] != pattern(f->id, p + j)) {
				fprintf(stderr, "koio %s: file %llu wrong at byte %llu %s\n", mode, (unsigned long long)f->id,
					(unsigned long long)(p + j), when);
				*bad = 1;
				return;
			}
	}
	int64_t z = l2m_zero_holes(fs, f->ino, NULL, NULL);
	CHECK("zero holes", z);
	if (z) {
		fprintf(stderr, "koio %s: file %llu has %lld block(s) of zero holes %s\n", mode, (unsigned long long)f->id,
			(long long)z, when);
		*bad = 1;
	}
}

static uint64_t count_extents(uint32_t ino, uint64_t size)
{
	uint64_t n = 0;
	l2m_extent ex[64];
	for (uint64_t lb = 0; lb < (size + bs - 1) / bs;) {
		int k = l2m_map(fs, ino, lb, (size + bs - 1) / bs - lb, 0, ex, 64);
		CHECK("map extents", k);
		for (int i = 0; i < k; i++, n++)
			lb = ex[i].lblk + ex[i].len;
	}
	return n;
}

/* a preallocated file: old data on the disk, one operation writes two
 * pages far apart and reports the whole span */
static int stale_case(void)
{
	uint32_t ino;
	CHECK("create", l2m_create(fs, L2M_ROOT_INO, "prealloc", 8, 0100644, 0, 0, 0, &ino));
	uint64_t size = 8 << 20, got;
	CHECK("preallocate", l2m_preallocate(fs, ino, 0, size, &got));
	l2m_stat st = {.size = size};
	CHECK("set size", l2m_setattr(fs, ino, L2M_SET_SIZE, &st));
	/* old contents of the disk under the whole file */
	l2m_extent ex[64];
	static uint8_t old[4096];
	memset(old, 0xEE, sizeof(old));
	for (uint64_t lb = 0; lb < size / bs;) {
		int n = l2m_map(fs, ino, lb, size / bs - lb, 0, ex, 64);
		CHECK("map", n);
		for (int i = 0; i < n; i++) {
			for (uint32_t k = 0; k < ex[i].len; k++)
				if (pwrite(dev_fd, old, bs, (off_t)((ex[i].pblk + k) * bs)) != (ssize_t)bs)
					return perror("pwrite"), 1;
			lb = ex[i].lblk + ex[i].len;
		}
	}
	struct sfile f = {.ino = ino, .id = 7, .size = size};
	f.dirty = calloc(size / PAGE + 1, 1);
	f.pblk = calloc(size / bs + 4, sizeof(uint64_t));
	f.dirty[10] = f.dirty[400] = 1; /* two pages, 6 MB apart */
	push(&f, 0, size / PAGE);
	CHECK("release", l2m_koio_release(fs, ino));
	int bad = 0;
	static uint8_t buf[PAGE];
	for (uint64_t pg = 0; pg < size / PAGE; pg++) {
		CHECK("read", l2m_read(fs, ino, pg * PAGE, buf, PAGE));
		for (uint64_t j = 0; j < PAGE; j++) {
			uint8_t want = pg == 10 || pg == 400 ? pattern(7, pg * PAGE + j) : 0;
			if (buf[j] != want) {
				fprintf(stderr, "koio stale: page %llu byte %llu reads %#x, want %#x%s\n", (unsigned long long)pg,
					(unsigned long long)j, buf[j], want, buf[j] == 0xEE ? " (old disk contents!)" : "");
				bad = 1;
				break;
			}
		}
		if (bad)
			break;
	}
	free(f.dirty);
	free(f.pblk);
	printf("koio stale: preallocated gap %s\n", bad ? "SHOWS OLD DATA" : "reads as zeros");
	return bad;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: koio IMAGE download|misreport|lost|stale|merge [seed]\n");
		return 2;
	}
	mode = argv[2];
	if (argc > 3)
		seed = (unsigned)atoi(argv[3]);
	dev_fd = open(argv[1], O_RDWR);
	if (dev_fd < 0) {
		perror(argv[1]);
		return 1;
	}
	l2m_dev dev;
	l2m_dev_open_fd(dev_fd, 1, &dev);
	char err[256];
	if (l2m_mount(&dev, 0, &fs, err, sizeof(err))) {
		fprintf(stderr, "mount: %s\n", err);
		return 1;
	}
	l2m_info info;
	CHECK("info", l2m_get_info(fs, &info));
	bs = info.block_size;
	int bad = 0;
	if (!strcmp(mode, "stale")) {
		bad = stale_case();
	} else {
		bool merge = !strcmp(mode, "merge");
		int nf = merge ? 1 : 4;
		uint64_t mb = getenv("L2M_KOIO_MB") ? (uint64_t)atoll(getenv("L2M_KOIO_MB")) : 160;
		struct sfile files[4];
		for (int i = 0; i < nf; i++) {
			struct sfile *f = &files[i];
			memset(f, 0, sizeof(*f));
			char name[32];
			snprintf(name, sizeof(name), "download-%d", i);
			CHECK("create", l2m_create(fs, L2M_ROOT_INO, name, strlen(name), 0100644, 0, 0, 0, &f->ino));
			f->id = (uint64_t)i + 1;
			f->target = (mb << 20) + rnd() % (1 << 20) + (uint64_t)i * 3333;
			f->dirty = calloc(f->target / PAGE + 2, 1);
			f->pblk = calloc(f->target / bs + 8, sizeof(uint64_t));
			f->next_sync = (rnd() % 64 + 1) << 20;
		}
		/* the downloads run at the same time, in odd-sized pieces */
		for (bool more = true; more;) {
			more = false;
			for (int i = 0; i < nf; i++) {
				struct sfile *f = &files[i];
				if (f->size >= f->target)
					continue;
				uint64_t n = rnd() % (merge ? 300000 : 140000) + 1;
				if (n > f->target - f->size)
					n = f->target - f->size;
				append(f, n);
				more = true;
			}
		}
		closing = true;
		for (int i = 0; i < nf; i++)
			push_all(&files[i]); /* close */
		/* lost reports are only made up for when the kernel releases the
		 * file, or once they are old (L2M_KOIO_LOST_MS) */
		if (strcmp(mode, "lost") || (getenv("L2M_KOIO_LOST_MS") && !atoi(getenv("L2M_KOIO_LOST_MS"))))
			for (int i = 0; i < nf; i++)
				check_file(&files[i], "before the kernel released it", &bad);
		uint64_t extents = 0, total = 0;
		for (int i = 0; i < nf; i++) {
			CHECK("release", l2m_koio_release(fs, files[i].ino));
			check_file(&files[i], "after the kernel released it", &bad);
			extents += count_extents(files[i].ino, files[i].size);
			total += files[i].size;
			free(files[i].dirty);
			free(files[i].pblk);
		}
		if (merge && extents > total / (128ull << 20) + 2) {
			fprintf(stderr, "koio merge: %llu extents for %llu MB written in order\n", (unsigned long long)extents,
				(unsigned long long)(total >> 20));
			bad = 1;
		}
		printf("koio %s: %d file(s), %llu MB, %llu operations, %llu extents, data %s\n", mode, nf,
		       (unsigned long long)(total >> 20), (unsigned long long)(opseq - 1000), (unsigned long long)extents,
		       bad ? "WRONG" : "ok");
	}
	char report[8192];
	int problems = l2m_verify(fs, NULL, NULL, report, sizeof(report));
	if (problems) {
		fprintf(stderr, "koio %s: l2m_verify found %d problem(s)\n", mode, problems);
		fputs(report, stderr);
	}
	int r = l2m_umount(fs);
	l2m_dev_close_fd(&dev);
	close(dev_fd);
	return bad || problems || r ? 1 : 0;
}
