/*
 * ExtBridge - big files on a fragmented volume, written the way macOS does
 * it with kernel-offloaded I/O.
 *
 *   bigfile IMAGE MODE [seed]
 *
 * The volume is first filled with small files and every second one is
 * deleted, so free space only comes in short runs and a big file needs
 * thousands of extents (several leaves, tree depth 2). Then one big file
 * is written:
 *
 *   prealloc  preallocate the whole file (unwritten extents, like Finder's
 *             F_PREALLOCATE), then write it front to back in chunks
 *   map       no preallocation: map each chunk for writing, then write it
 *   shuffle   preallocate, then write the chunks in random order with
 *             ranges that start and end inside blocks
 *   boundary  small targeted cases: data written into a long unwritten
 *             extent of a full leaf (start, middle, end, all of it)
 *   kernel    map 16 MB for writing at a time, but report the data as
 *             written in smaller, odd-sized (but block-aligned) pieces
 *
 * "Writing" a chunk is what the kernel does: ask l2m_map for the blocks,
 * put the data straight onto the device, then report l2m_write_done.
 * Afterwards the file is read back through the core and compared, and
 * l2m_verify must find nothing; tests/run_all.sh runs e2fsck on the image.
 *
 * SPDX-License-Identifier: MIT
 */
#define _FILE_OFFSET_BITS 64
#include "l2m/l2m.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static l2m_fs *fs;
static int dev_fd;
static uint32_t bs;
static uint64_t done_piece; /* 0: one l2m_write_done per write */

static uint8_t pattern(uint64_t off) { return (uint8_t)((off * 40503u) ^ (off >> 9) ^ 0x5a); }

#define CHECK(what, expr)                                                                                   \
	do {                                                                                                \
		int r_ = (expr);                                                                            \
		if (r_ < 0) {                                                                               \
			fprintf(stderr, "bigfile: %s failed: %d (%s)\n", what, r_, strerror(-r_));         \
			exit(1);                                                                            \
		}                                                                                           \
	} while (0)

static unsigned seed = 1;
static unsigned rnd(void)
{
	seed = seed * 1103515245u + 12345u;
	return seed >> 8;
}

/* fill the volume with small files, then delete every second one */
static void fragment(uint32_t dir)
{
	uint8_t buf[8192];
	memset(buf, 0x11, sizeof(buf));
	char name[32];
	int n = 0;
	for (;; n++) {
		uint32_t ino;
		snprintf(name, sizeof(name), "f%06d", n);
		int r = l2m_create(fs, dir, name, strlen(name), 0100644, 0, 0, 0, &ino);
		if (r == -ENOSPC)
			break;
		CHECK("create small file", r);
		int64_t w = l2m_write(fs, ino, 0, buf, sizeof(buf));
		if (w == -ENOSPC)
			break;
		CHECK("write small file", (int)(w < 0 ? w : 0));
	}
	for (int i = 0; i < n; i += 2) {
		snprintf(name, sizeof(name), "f%06d", i);
		int orphan = 0;
		uint32_t ino;
		CHECK("lookup small file", l2m_lookup(fs, dir, name, strlen(name), &ino));
		CHECK("delete small file", l2m_unlink(fs, dir, name, strlen(name), 0, &orphan));
		if (orphan)
			CHECK("release small file", l2m_release(fs, ino));
	}
	CHECK("sync", l2m_sync(fs));
}

/* what the kernel does for one write: map, put the data on the device, done */
static void kernel_write(uint32_t ino, uint64_t off, uint64_t len, uint64_t *size)
{
	uint64_t first = off / bs, last = (off + len + bs - 1) / bs;
	l2m_extent ex[64];
	uint64_t lblk = first;
	while (lblk < last) {
		int n = l2m_map(fs, ino, lblk, last - lblk, L2M_MAP_ALLOCATE, ex, 64);
		CHECK("map for writing", n);
		for (int i = 0; i < n; i++) {
			if (ex[i].type == L2M_EXT_HOLE) {
				fprintf(stderr, "bigfile: hole after allocation at block %llu\n", (unsigned long long)ex[i].lblk);
				exit(1);
			}
			/* write the covered bytes of this extent */
			uint64_t a = ex[i].lblk * bs, b = (ex[i].lblk + ex[i].len) * bs;
			uint64_t from = a > off ? a : off, to = b < off + len ? b : off + len;
			for (uint64_t p = from; p < to;) {
				uint8_t buf[65536];
				size_t k = (size_t)(to - p < sizeof(buf) ? to - p : sizeof(buf));
				for (size_t j = 0; j < k; j++)
					buf[j] = pattern(p + j);
				off_t phys = (off_t)(ex[i].pblk * bs + (p - a));
				if (pwrite(dev_fd, buf, k, phys) != (ssize_t)k) {
					perror("pwrite");
					exit(1);
				}
				p += k;
			}
			lblk = ex[i].lblk + ex[i].len;
		}
		if (n == 0)
			break;
	}
	if (off + len > *size)
		*size = off + len;
	if (done_piece == 0) {
		CHECK("write done", l2m_write_done(fs, ino, off, len, *size));
		return;
	}
	/* the kernel maps big ranges but reports them done in smaller pieces */
	for (uint64_t p = off; p < off + len; p += done_piece) {
		uint64_t k = off + len - p < done_piece ? off + len - p : done_piece;
		CHECK("write done (piece)", l2m_write_done(fs, ino, p, k, p + k > *size ? p + k : *size));
	}
}

/* A full leaf with one long unwritten extent among single-block ones, then
 * data written into that extent: at its start, middle or end, or all of
 * it. The conversion needs up to two more entries in a leaf without room;
 * the long extent sits first, in the middle, second to last or last. */
static int boundary_case(int pos, uint64_t from, uint64_t count, int *bad)
{
	uint32_t ino;
	l2m_extent ex[4];
	char name[32];
	snprintf(name, sizeof(name), "e%d-%llu-%llu", pos, (unsigned long long)from, (unsigned long long)count);
	CHECK("create", l2m_create(fs, L2M_ROOT_INO, name, strlen(name), 0100644, 0, 0, 0, &ino));
	int per_leaf = (int)((bs - 12) / 12);
	uint64_t lblk = 0, longx = 0;
	for (int i = 0; i < per_leaf; i++) {
		/* holes between the extents keep them apart */
		if (i == pos) {
			longx = lblk;
			CHECK("map the long extent", l2m_map(fs, ino, lblk, 10, L2M_MAP_ALLOCATE, ex, 4));
			lblk += 12;
		} else {
			CHECK("map one block", l2m_map(fs, ino, lblk, 1, L2M_MAP_ALLOCATE, ex, 4));
			lblk += 2;
		}
	}
	/* the next extent starts a new leaf */
	CHECK("map beyond", l2m_map(fs, ino, lblk + 100, 1, L2M_MAP_ALLOCATE, ex, 4));
	uint64_t size = (lblk + 101) * bs;
	uint8_t buf[4096], back[4096];
	for (uint64_t k = 0; k < count; k++) {
		int n = l2m_map(fs, ino, longx + from + k, 1, L2M_MAP_ALLOCATE, ex, 4);
		CHECK("map", n);
		memset(buf, (int)(0x30 + k), sizeof(buf));
		if (pwrite(dev_fd, buf, bs, (off_t)(ex[0].pblk * bs)) != (ssize_t)bs)
			return perror("pwrite"), 1;
	}
	CHECK("write done", l2m_write_done(fs, ino, (longx + from) * bs, count * bs, size));
	for (uint64_t k = 0; k < 10; k++) {
		int64_t n = l2m_read(fs, ino, (longx + k) * bs, back, bs);
		memset(buf, k >= from && k < from + count ? (int)(0x30 + k - from) : 0, sizeof(buf));
		if (n != (int64_t)bs || memcmp(back, buf, bs)) {
			fprintf(stderr, "bigfile boundary %s: block %llu of the long extent reads wrong\n", name, (unsigned long long)k);
			*bad = 1;
		}
	}
	return 0;
}

static int boundary(const char *image, l2m_dev *dev)
{
	(void)image;
	int per_leaf = (int)((bs - 12) / 12), bad = 0;
	int positions[] = {0, per_leaf / 2, per_leaf - 2, per_leaf - 1};
	uint64_t ranges[][2] = {{0, 1}, {0, 4}, {5, 1}, {3, 4}, {9, 1}, {6, 4}, {0, 10}};
	int cases = 0;
	for (size_t p = 0; p < sizeof(positions) / sizeof(*positions); p++)
		for (size_t r = 0; r < sizeof(ranges) / sizeof(*ranges); r++, cases++)
			if (boundary_case(positions[p], ranges[r][0], ranges[r][1], &bad))
				return 1;
	char report[8192];
	int problems = l2m_verify(fs, NULL, NULL, report, sizeof(report));
	printf("bigfile boundary: %d cases, data %s, %d problem(s)\n", cases, bad ? "WRONG" : "ok", problems);
	if (problems)
		fputs(report, stderr);
	int r = l2m_umount(fs);
	l2m_dev_close_fd(dev);
	close(dev_fd);
	return bad || problems || r ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: bigfile IMAGE prealloc|map|shuffle|kernel|boundary [seed]\n");
		return 2;
	}
	const char *mode = argv[2];
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

	uint32_t root = L2M_ROOT_INO, frag, big;
	if (!strcmp(mode, "boundary"))
		return boundary(argv[1], &dev);
	CHECK("mkdir", l2m_create(fs, root, "frag", 4, 040755, 0, 0, 0, &frag));
	fragment(frag);
	l2m_statfs st;
	CHECK("statfs", l2m_statfs_get(fs, &st));
	/* the big file takes most of the remaining (fragmented) space */
	uint64_t size = st.bfree * bs * 8 / 10;
	CHECK("create big file", l2m_create(fs, root, "big", 3, 0100644, 0, 0, 0, &big));

	uint64_t chunk = 1 << 20, cur = 0;
	if (!strcmp(mode, "kernel")) {
		/* map 16 MB at a time, report done in 1 MB + odd pieces */
		chunk = 16 << 20;
		done_piece = (1 << 20) + 4096 * 3; /* the kernel completes whole pages */
	}
	if (strcmp(mode, "map") && strcmp(mode, "kernel")) {
		uint64_t got = 0;
		CHECK("preallocate", l2m_preallocate(fs, big, 0, size, &got));
	}
	if (!strcmp(mode, "shuffle")) {
		uint64_t n = (size + chunk - 1) / chunk;
		uint64_t *order = malloc(n * sizeof(*order));
		for (uint64_t i = 0; i < n; i++)
			order[i] = i;
		for (uint64_t i = n - 1; i > 0; i--) {
			uint64_t j = rnd() % (i + 1), t = order[i];
			order[i] = order[j];
			order[j] = t;
		}
		/* uneven pieces: each chunk in two writes that split inside a block */
		for (uint64_t i = 0; i < n; i++) {
			uint64_t a = order[i] * chunk, b = a + chunk < size ? a + chunk : size;
			uint64_t mid = a + (rnd() % (b - a));
			if (mid > a)
				kernel_write(big, a, mid - a, &cur);
			kernel_write(big, mid, b - mid, &cur);
		}
		free(order);
	} else {
		for (uint64_t a = 0; a < size; a += chunk)
			kernel_write(big, a, a + chunk < size ? chunk : size - a, &cur);
	}

	/* read back through the core */
	uint8_t buf[65536];
	for (uint64_t p = 0; p < size; p += sizeof(buf)) {
		size_t k = (size_t)(size - p < sizeof(buf) ? size - p : sizeof(buf));
		int64_t n = l2m_read(fs, big, p, buf, k);
		CHECK("read back", (int)(n < 0 ? n : 0));
		for (size_t j = 0; j < k; j++) {
			if (buf[j] != pattern(p + j)) {
				fprintf(stderr, "bigfile: wrong byte at offset %llu\n", (unsigned long long)(p + j));
				return 1;
			}
		}
	}
	char report[8192];
	int problems = l2m_verify(fs, NULL, NULL, report, sizeof(report));
	l2m_stat bst;
	CHECK("stat", l2m_getattr(fs, big, &bst));
	printf("bigfile %s: %llu MB in %llu blocks, %d problem(s)\n", mode, (unsigned long long)(size >> 20),
	       (unsigned long long)(bst.alloc / bs), problems);
	if (problems)
		fputs(report, stderr);
	int r = l2m_umount(fs);
	l2m_dev_close_fd(&dev);
	close(dev_fd);
	return problems || r ? 1 : 0;
}
