/*
 * ExtBridge - concurrency stress test for the core.
 *
 *   stress IMAGE [threads] [seconds]
 *
 * Several threads work at the same time on one mounted volume (like the
 * Finder copying many files): each thread owns a directory and creates,
 * writes, reads back and verifies, renames, truncates, links and deletes
 * files with random sizes; one extra thread keeps listing directories and
 * reading statistics. Half of the files are written the way macOS does it
 * (kernel-offloaded I/O: l2m_koio_map, data straight onto the device,
 * l2m_koio_done). Every read is checked against the expected pattern.
 * Afterwards the volume is unmounted; tests/run_all.sh runs e2fsck on it.
 *
 * SPDX-License-Identifier: MIT
 */
#define _FILE_OFFSET_BITS 64
#include "l2m/l2m.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static l2m_fs *fs;
static int dev_fd;
static uint32_t bs;
static atomic_ullong opseq;
static atomic_int stop;
static atomic_long ops, errors;
static int seconds = 20;

static uint8_t pattern(uint32_t seed, uint64_t off) { return (uint8_t)((seed * 2654435761u) ^ (off * 40503u) ^ (off >> 7)); }

static atomic_long full;

static void fail(const char *what, int r, int tid)
{
	if (r == -ENOSPC) { /* a full volume is a normal condition */
		atomic_fetch_add(&full, 1);
		return;
	}
	fprintf(stderr, "thread %d: %s failed: %d (%s)\n", tid, what, r, strerror(r < 0 ? -r : r));
	atomic_fetch_add(&errors, 1);
}

struct worker {
	int id;
	unsigned rnd;
};

static unsigned rnd(struct worker *w)
{
	w->rnd = w->rnd * 1103515245u + 12345u;
	return w->rnd >> 8;
}

/* the kernel's way: map (16 extents at a time), write whole blocks from its
 * cache straight onto the device, report the operation done */
static int64_t koio_write(struct worker *w, uint32_t ino, const uint8_t *buf, uint64_t off, uint64_t len)
{
	uint64_t op = atomic_fetch_add(&opseq, 1) + 1, first = off / bs, last = (off + len + bs - 1) / bs;
	uint8_t blk[65536];
	for (uint64_t lb = first; lb < last;) {
		l2m_extent ex[16];
		int n = l2m_koio_map(fs, ino, op, lb, last - lb, L2M_MAP_ALLOCATE, ex, 16);
		if (n <= 0)
			return n ? n : -EIO;
		for (int i = 0; i < n; i++) {
			for (uint32_t k = 0; k < ex[i].len; k++) {
				uint64_t b = ex[i].lblk + k;
				for (uint32_t j = 0; j < bs; j++)
					blk[j] = b * bs + j < off + len ? buf[b * bs + j] : 0;
				if (pwrite(dev_fd, blk, bs, (off_t)((ex[i].pblk + k) * bs)) != (ssize_t)bs)
					return -errno;
			}
			lb = ex[i].lblk + ex[i].len;
		}
	}
	int r = l2m_koio_done(fs, ino, op, first * bs, off + len - first * bs, 1);
	if (!r && rnd(w) % 4 == 0)
		r = l2m_koio_release(fs, ino);
	return r ? r : (int64_t)len;
}

static void *work(void *arg)
{
	struct worker *w = arg;
	char dname[32];
	snprintf(dname, sizeof(dname), "t%02d", w->id);
	uint32_t dir;
	int r = l2m_create(fs, L2M_ROOT_INO, dname, strlen(dname), 0040755, 1000, 1000, 0, &dir);
	if (r) {
		fail("mkdir", r, w->id);
		return NULL;
	}
	uint8_t *buf = malloc(4 << 20), *chk = malloc(4 << 20);
	int n = 0;
	while (!atomic_load(&stop)) {
		char name[64], name2[64];
		snprintf(name, sizeof(name), "f%06d", n);
		snprintf(name2, sizeof(name2), "renamed_%06d_with_a_longer_name", n);
		n++;
		uint32_t ino, seed = rnd(w);
		uint64_t size = (uint64_t[]){0, 1, 4095, 4096, 4097, 70000, 1 << 20, 3 << 20}[rnd(w) % 8];
		r = l2m_create(fs, dir, name, strlen(name), 0100644, 1000, 1000, 0, &ino);
		if (r) {
			fail("create", r, w->id);
			break;
		}
		for (uint64_t i = 0; i < size; i++)
			buf[i] = pattern(seed, i);
		/* write in random chunks */
		bool koio = rnd(w) % 2;
		for (uint64_t off = 0; off < size;) {
			uint64_t len = 1 + rnd(w) % (size - off < 300000 ? size - off : 300000);
			int64_t wr = koio ? koio_write(w, ino, buf, off, len) : l2m_write(fs, ino, off, buf + off, len);
			if (wr == -ENOSPC || (wr >= 0 && wr < (int64_t)len)) {
				/* volume full: clean up this file and keep going */
				int orphan = 0;
				l2m_unlink(fs, dir, name, strlen(name), 0, &orphan);
				if (orphan)
					l2m_release(fs, ino);
				atomic_fetch_add(&full, 1);
				goto next;
			}
			if (wr != (int64_t)len) {
				fail("write", (int)wr, w->id);
				goto out;
			}
			off += len;
		}
		int64_t rd = l2m_read(fs, ino, 0, chk, size + 100);
		if (rd != (int64_t)size || memcmp(buf, chk, size) != 0) {
			fail("read-back compare", (int)rd, w->id);
			goto out;
		}
		switch (rnd(w) % 8) {
		case 0: /* rename */
			r = l2m_rename(fs, dir, name, strlen(name), dir, name2, strlen(name2), 0, NULL, NULL);
			if (r)
				fail("rename", r, w->id);
			break;
		case 1: { /* truncate and check zeros */
			uint64_t ns = size / 3;
			r = l2m_truncate(fs, ino, ns);
			if (!r)
				r = l2m_truncate(fs, ino, ns + 5000);
			if (r) {
				fail("truncate", r, w->id);
				break;
			}
			rd = l2m_read(fs, ino, 0, chk, ns + 5000);
			for (uint64_t i = ns; i < ns + 5000 && rd == (int64_t)(ns + 5000); i++)
				if (chk[i]) {
					fail("truncate zero check", -EIO, w->id);
					break;
				}
			break;
		}
		case 2: /* hard link + unlink original */
			r = l2m_link(fs, ino, dir, name2, strlen(name2));
			if (!r)
				r = l2m_unlink(fs, dir, name, strlen(name), 0, NULL);
			if (r)
				fail("link", r, w->id);
			break;
		case 3:
		case 5:
		case 6:
		case 7: { /* delete again */
			int orphan = 0;
			r = l2m_unlink(fs, dir, name, strlen(name), 0, &orphan);
			if (!r && orphan)
				r = l2m_release(fs, ino);
			if (r)
				fail("unlink", r, w->id);
			break;
		}
		default: /* xattr */
			r = l2m_setxattr(fs, ino, "com.apple.FinderInfo", buf, 32, 0);
			if (r)
				fail("setxattr", r, w->id);
			break;
		}
		atomic_fetch_add(&ops, 1);
	next:;
	}
out:
	free(buf);
	free(chk);
	return NULL;
}

static int count_cb(void *ctx, const char *name, size_t len, uint32_t ino, unsigned t, uint64_t next)
{
	(*(long *)ctx)++;
	return 0;
}

static void *watcher(void *arg)
{
	(void)arg;
	while (!atomic_load(&stop)) {
		l2m_statfs st;
		l2m_statfs_get(fs, &st);
		for (int t = 0; t < 64; t++) {
			char d[16];
			uint32_t ino;
			snprintf(d, sizeof(d), "t%02d", t);
			if (l2m_lookup(fs, L2M_ROOT_INO, d, strlen(d), &ino))
				continue;
			long c = 0;
			uint64_t cookie = 0;
			int r = l2m_readdir(fs, ino, &cookie, count_cb, &c);
			if (r)
				fail("readdir", r, 99);
		}
		l2m_sync_if_older(fs, 2000);
		usleep(20000);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: stress IMAGE [threads] [seconds]\n");
		return 2;
	}
	int nthreads = argc > 2 ? atoi(argv[2]) : 8;
	if (argc > 3)
		seconds = atoi(argv[3]);
	int fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	dev_fd = fd;
	l2m_dev dev;
	l2m_dev_open_fd(fd, 1, &dev);
	char err[256];
	int r = l2m_mount(&dev, 0, &fs, err, sizeof(err));
	if (r) {
		fprintf(stderr, "mount: %s\n", err);
		return 1;
	}
	l2m_info info;
	l2m_get_info(fs, &info);
	bs = info.block_size;
	pthread_t th[64], wt;
	struct worker w[64];
	for (int i = 0; i < nthreads && i < 64; i++) {
		w[i].id = i;
		w[i].rnd = (unsigned)(i * 7919 + time(NULL));
		pthread_create(&th[i], NULL, work, &w[i]);
	}
	pthread_create(&wt, NULL, watcher, NULL);
	sleep((unsigned)seconds);
	atomic_store(&stop, 1);
	for (int i = 0; i < nthreads && i < 64; i++)
		pthread_join(th[i], NULL);
	pthread_join(wt, NULL);
	r = l2m_umount(fs);
	l2m_dev_close_fd(&dev);
	close(fd);
	printf("stress: %d threads, %ld file cycles, %ld times volume full (handled), %ld errors, unmount %d\n", nthreads, atomic_load(&ops), atomic_load(&full), atomic_load(&errors), r);
	return atomic_load(&errors) || r ? 1 : 0;
}
