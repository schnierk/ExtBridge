/*
 * ExtBridge - block device on top of a file descriptor (image files and
 * raw devices). Used by the command line tool and the tests.
 *
 * SPDX-License-Identifier: MIT
 */
#define _FILE_OFFSET_BITS 64
#include "internal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/disk.h>
#include <sys/ioctl.h>
#elif defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

/* Power-loss simulation for the crash tests (L2M_CRASH_AFTER_WRITES=n):
 * all writes since the last flush are kept in a log. At the crash point a
 * random subset of them (per 4 KiB piece, in any order) is "lost", like a
 * disk cache that was never flushed, then the process dies at once. */
struct wlog {
	uint64_t off;
	uint32_t len;
	uint8_t *data; /* new content */
	uint8_t *old;  /* content before this write */
};

struct fdctx {
	int fd;
	int writable;
	/* fault injection for tests: fail after this many writes (0 = off) */
	long fail_after;
	long crash_after;
	long writes;
	unsigned seed;
	struct wlog *log;
	size_t nlog, caplog;
};

static unsigned rnd(struct fdctx *x)
{
	x->seed = x->seed * 1103515245u + 12345u;
	return (x->seed >> 16) & 0x7fff;
}

static int raw_pwrite(int fd, const uint8_t *p, size_t len, uint64_t off)
{
	while (len) {
		ssize_t n = pwrite(fd, p, len, (off_t)off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		p += n;
		off += (uint64_t)n;
		len -= (size_t)n;
	}
	return 0;
}

static void crash_now(struct fdctx *x)
{
	/* undo everything since the last flush (newest first), then replay
	 * a random subset of the 4 KiB pieces in order */
	for (size_t i = x->nlog; i-- > 0;)
		raw_pwrite(x->fd, x->log[i].old, x->log[i].len, x->log[i].off);
	size_t kept = 0, lost = 0;
	for (size_t i = 0; i < x->nlog; i++)
		for (uint32_t o = 0; o < x->log[i].len; o += 4096) {
			uint32_t l = x->log[i].len - o < 4096 ? x->log[i].len - o : 4096;
			if (rnd(x) & 1) {
				raw_pwrite(x->fd, x->log[i].data + o, l, x->log[i].off + o);
				kept++;
			} else {
				lost++;
			}
		}
	fsync(x->fd);
	fprintf(stderr, "l2m-test: simulated power loss after %ld writes (%zu pieces kept, %zu lost)\n", x->writes, kept, lost);
	_exit(99);
}

static void log_clear(struct fdctx *x)
{
	for (size_t i = 0; i < x->nlog; i++) {
		free(x->log[i].data);
		free(x->log[i].old);
	}
	x->nlog = 0;
}

static int fd_read(void *c, uint64_t off, void *buf, size_t len)
{
	struct fdctx *x = c;
	uint8_t *p = buf;
	while (len) {
		ssize_t n = pread(x->fd, p, len, (off_t)off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;
		p += n;
		off += (uint64_t)n;
		len -= (size_t)n;
	}
	return 0;
}

static int fd_write(void *c, uint64_t off, const void *buf, size_t len)
{
	struct fdctx *x = c;
	const uint8_t *p = buf;
	if (!x->writable)
		return -EROFS;
	x->writes++;
	if (x->fail_after && x->writes > x->fail_after)
		return -EIO; /* simulated dead disk: nothing more reaches it */
	if (x->crash_after) {
		if (x->writes > x->crash_after)
			crash_now(x);
		if (x->nlog == x->caplog) {
			x->caplog = x->caplog ? x->caplog * 2 : 256;
			x->log = realloc(x->log, x->caplog * sizeof(*x->log));
		}
		struct wlog *w = &x->log[x->nlog++];
		w->off = off;
		w->len = (uint32_t)len;
		w->data = malloc(len);
		w->old = calloc(1, len);
		memcpy(w->data, p, len);
		if (pread(x->fd, w->old, len, (off_t)off) < 0)
			memset(w->old, 0, len);
	}
	return raw_pwrite(x->fd, p, len, off);
}

static int fd_flush(void *c)
{
	struct fdctx *x = c;
	if (!x->writable)
		return 0;
	if (x->fail_after && x->writes >= x->fail_after)
		return -EIO;
	if (x->crash_after)
		log_clear(x); /* everything so far is durable now */
	if (getenv("L2M_NO_FSYNC"))
		return 0;
#ifdef __APPLE__
	if (fcntl(x->fd, F_FULLFSYNC) == 0)
		return 0;
#endif
	return fsync(x->fd) == 0 ? 0 : -errno;
}

int l2m_dev_open_fd(int fd, int writable, l2m_dev *dev)
{
	struct stat st;
	if (fstat(fd, &st) != 0)
		return -errno;
	struct fdctx *x = calloc(1, sizeof(*x));
	if (!x)
		return -ENOMEM;
	x->fd = fd;
	x->writable = writable;
	const char *f = getenv("L2M_FAIL_AFTER_WRITES");
	if (f)
		x->fail_after = atol(f);
	const char *c = getenv("L2M_CRASH_AFTER_WRITES");
	if (c && writable) {
		x->crash_after = atol(c);
		const char *sd = getenv("L2M_CRASH_SEED");
		x->seed = sd ? (unsigned)atol(sd) : 1;
	}
	memset(dev, 0, sizeof(*dev));
	dev->ctx = x;
	dev->read = fd_read;
	dev->write = fd_write;
	dev->flush = fd_flush;
	dev->sector_size = 512;
	if (S_ISREG(st.st_mode)) {
		dev->size = (uint64_t)st.st_size;
	} else {
#ifdef __APPLE__
		uint64_t count = 0;
		uint32_t bsz = 0;
		if (ioctl(fd, DKIOCGETBLOCKCOUNT, &count) == 0 && ioctl(fd, DKIOCGETBLOCKSIZE, &bsz) == 0) {
			dev->size = count * bsz;
			dev->sector_size = bsz;
		}
#elif defined(__linux__)
		uint64_t sz = 0;
		if (ioctl(fd, BLKGETSIZE64, &sz) == 0)
			dev->size = sz;
#endif
		if (!dev->size) {
			off_t e = lseek(fd, 0, SEEK_END);
			dev->size = e > 0 ? (uint64_t)e : 0;
		}
	}
	return 0;
}

void l2m_dev_close_fd(l2m_dev *dev)
{
	struct fdctx *x = dev->ctx;
	if (x && getenv("L2M_COUNT_WRITES"))
		fprintf(stderr, "l2m-test: writes=%ld\n", x->writes);
	if (x) {
		log_clear(x);
		free(x->log);
	}
	free(dev->ctx);
	dev->ctx = NULL;
}
