/*
 * ExtBridge - writes files the way a browser saves downloads, for testing a
 * mounted volume (macOS kernel I/O through FSKit, or anything else).
 *
 *   dlsim write DIR NFILES SIZE_MB [options]   one thread per file
 *       -r MB/s    pace every writer (default: as fast as possible)
 *       -a BYTES   smallest write (default 1)
 *       -b BYTES   largest write (default 4 MiB)
 *       -k K       K writers per file, each fills one slice (parallel download)
 *       -s SEED    first file id (default 1)
 *       -f         fsync before close
 *       -n         F_NOCACHE (macOS: write around the page cache where aligned)
 *   dlsim check FILE ID                        compare a file with its pattern
 *
 * Every byte depends on the file id and its offset, so "check" finds any
 * byte that is wrong; ranges are reported in 4 KiB blocks, with a note when
 * a range reads as zeros.
 *
 * SPDX-License-Identifier: MIT
 */
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static uint64_t mix(uint64_t x)
{
	x += 0x9e3779b97f4a7c15ull;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
	return x ^ (x >> 31);
}

static void fill(uint8_t *buf, uint64_t id, uint64_t off, size_t n)
{
	size_t i = 0;
	while (i < n) {
		uint64_t w = mix((id << 44) ^ ((off + i) >> 3));
		for (unsigned b = (unsigned)((off + i) & 7); b < 8 && i < n; b++, i++)
			buf[i] = (uint8_t)(w >> (8 * b));
	}
}

static double now(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

struct job {
	const char *path;
	uint64_t id, from, to;
	double rate;
	uint64_t min, max;
	int do_fsync, fd;
	unsigned seed;
	uint64_t writes;
};

static uint64_t chunk(struct job *j)
{
	/* log-uniform between min and max: many small and some big writes */
	double lo = log((double)j->min), hi = log((double)j->max);
	double u = (double)rand_r(&j->seed) / RAND_MAX;
	uint64_t n = (uint64_t)exp(lo + u * (hi - lo));
	return n < j->min ? j->min : n > j->max ? j->max : n;
}

static void *writer(void *arg)
{
	struct job *j = arg;
	uint8_t *buf = malloc(j->max);
	double t0 = now();
	for (uint64_t off = j->from; off < j->to;) {
		uint64_t n = chunk(j);
		if (n > j->to - off)
			n = j->to - off;
		fill(buf, j->id, off, (size_t)n);
		for (uint64_t done = 0; done < n;) {
			ssize_t w = pwrite(j->fd, buf + done, (size_t)(n - done), (off_t)(off + done));
			if (w < 0) {
				fprintf(stderr, "dlsim: %s: write at %" PRIu64 ": %s\n", j->path, off + done, strerror(errno));
				exit(1);
			}
			done += (uint64_t)w;
		}
		off += n;
		j->writes++;
		if (j->rate > 0) {
			double ahead = (double)(off - j->from) / (j->rate * 1048576.0) - (now() - t0);
			if (ahead > 0.001)
				usleep((useconds_t)(ahead * 1e6));
		}
	}
	free(buf);
	return NULL;
}

static int cmd_write(int argc, char **argv)
{
	if (argc < 5)
		return 2;
	const char *dir = argv[2];
	int nfiles = atoi(argv[3]);
	uint64_t size = (uint64_t)atoll(argv[4]) << 20;
	double rate = 0;
	uint64_t min = 1, max = 4 << 20, first = 1;
	int k = 1, do_fsync = 0, nocache = 0;
	for (int i = 5; i < argc; i++) {
		if (!strcmp(argv[i], "-r") && i + 1 < argc)
			rate = atof(argv[++i]);
		else if (!strcmp(argv[i], "-a") && i + 1 < argc)
			min = (uint64_t)atoll(argv[++i]);
		else if (!strcmp(argv[i], "-b") && i + 1 < argc)
			max = (uint64_t)atoll(argv[++i]);
		else if (!strcmp(argv[i], "-k") && i + 1 < argc)
			k = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-s") && i + 1 < argc)
			first = (uint64_t)atoll(argv[++i]);
		else if (!strcmp(argv[i], "-f"))
			do_fsync = 1;
		else if (!strcmp(argv[i], "-n"))
			nocache = 1;
		else
			return 2;
	}
	if (nfiles < 1 || k < 1 || min < 1 || max < min)
		return 2;
	struct job *jobs = calloc((size_t)(nfiles * k), sizeof(*jobs));
	pthread_t *th = calloc((size_t)(nfiles * k), sizeof(*th));
	int *fds = calloc((size_t)nfiles, sizeof(int));
	char (*paths)[1024] = calloc((size_t)nfiles, 1024);
	double t0 = now();
	for (int f = 0; f < nfiles; f++) {
		snprintf(paths[f], 1024, "%s/dl-%" PRIu64 ".bin", dir, first + (uint64_t)f);
		fds[f] = open(paths[f], O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fds[f] < 0) {
			fprintf(stderr, "dlsim: %s: %s\n", paths[f], strerror(errno));
			return 1;
		}
#ifdef F_NOCACHE
		if (nocache && fcntl(fds[f], F_NOCACHE, 1) != 0)
			fprintf(stderr, "dlsim: F_NOCACHE: %s\n", strerror(errno));
#else
		(void)nocache;
#endif
		uint64_t slice = (size / (uint64_t)k + 4095) & ~4095ull;
		for (int s = 0; s < k; s++) {
			struct job *j = &jobs[f * k + s];
			j->path = paths[f];
			j->id = first + (uint64_t)f;
			j->from = (uint64_t)s * slice < size ? (uint64_t)s * slice : size;
			j->to = s == k - 1 ? size : ((uint64_t)(s + 1) * slice < size ? (uint64_t)(s + 1) * slice : size);
			j->rate = rate;
			j->min = min;
			j->max = max;
			j->fd = fds[f];
			j->seed = (unsigned)(j->id * 7919 + (uint64_t)s);
			pthread_create(&th[f * k + s], NULL, writer, j);
		}
	}
	uint64_t writes = 0;
	for (int i = 0; i < nfiles * k; i++) {
		pthread_join(th[i], NULL);
		writes += jobs[i].writes;
	}
	for (int f = 0; f < nfiles; f++) {
		if (do_fsync && fsync(fds[f]) != 0)
			fprintf(stderr, "dlsim: fsync %s: %s\n", paths[f], strerror(errno));
		if (close(fds[f]) != 0)
			fprintf(stderr, "dlsim: close %s: %s\n", paths[f], strerror(errno));
	}
	double dt = now() - t0;
	printf("wrote %d file(s) of %" PRIu64 " MB in %" PRIu64 " writes, %.1f s, %.0f MB/s\n", nfiles, size >> 20, writes, dt,
	       (double)nfiles * (double)(size >> 20) / dt);
	return 0;
}

static int cmd_check(int argc, char **argv)
{
	if (argc != 4)
		return 2;
	uint64_t id = (uint64_t)atoll(argv[3]);
	int fd = !strcmp(argv[2], "-") ? 0 : open(argv[2], O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "dlsim: %s: %s\n", argv[2], strerror(errno));
		return 1;
	}
	enum { B = 4096, N = 256 };
	static uint8_t got[B * N], want[B * N];
	uint64_t off = 0, bad_blocks = 0, ranges = 0;
	int64_t run_start = -1;
	int run_zero = 1;
	for (;;) {
		ssize_t n = 0;
		while (n < (ssize_t)sizeof(got)) {
			ssize_t r = read(fd, got + n, sizeof(got) - (size_t)n);
			if (r < 0) {
				fprintf(stderr, "dlsim: read at %" PRIu64 ": %s\n", off + (uint64_t)n, strerror(errno));
				return 1;
			}
			if (r == 0)
				break;
			n += r;
		}
		if (n == 0)
			break;
		fill(want, id, off, (size_t)n);
		for (ssize_t p = 0; p < n; p += B) {
			size_t len = (size_t)(n - p < B ? n - p : B);
			uint64_t blk = (off + (uint64_t)p) / B;
			bool bad = memcmp(got + p, want + p, len) != 0;
			if (bad) {
				bad_blocks++;
				bool zero = true;
				for (size_t q = 0; q < len && zero; q++)
					zero = got[p + q] == 0;
				if (run_start < 0) {
					run_start = (int64_t)blk;
					run_zero = zero;
				} else if (!zero) {
					run_zero = 0;
				}
			} else if (run_start >= 0) {
				if (++ranges <= 20)
					printf("BAD blocks %" PRId64 "-%" PRIu64 " (%" PRIu64 " blocks)%s\n", run_start, blk - 1, blk - (uint64_t)run_start,
					       run_zero ? " zeros" : "");
				run_start = -1;
			}
		}
		off += (uint64_t)n;
	}
	if (run_start >= 0 && ++ranges <= 20)
		printf("BAD blocks %" PRId64 "-%" PRIu64 "%s\n", run_start, (off + B - 1) / B - 1, run_zero ? " zeros" : "");
	printf("%s: %" PRIu64 " bytes, %" PRIu64 " bad block(s) in %" PRIu64 " range(s)\n", argv[2], off, bad_blocks, ranges);
	return bad_blocks ? 3 : 0;
}

int main(int argc, char **argv)
{
	int r = 2;
	if (argc > 1 && !strcmp(argv[1], "write"))
		r = cmd_write(argc, argv);
	else if (argc > 1 && !strcmp(argv[1], "check"))
		r = cmd_check(argc, argv);
	if (r == 2)
		fprintf(stderr, "usage: dlsim write DIR NFILES SIZE_MB [-r MB/s] [-a MIN] [-b MAX] [-k SLICES] [-s ID] [-f] [-n]\n"
				"       dlsim check FILE|- ID\n");
	return r;
}
