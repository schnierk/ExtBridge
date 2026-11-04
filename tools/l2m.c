/*
 * l2m - command line access to ext2/3/4 images and disks via libl2m.
 *
 *   l2m [-w] [-s] <image|device> <command> [args...]
 *
 * Without -w the volume is opened strictly read-only (the device is opened
 * O_RDONLY, so not a single byte can be written).
 *
 * SPDX-License-Identifier: MIT
 */
#define _FILE_OFFSET_BITS 64
#include "l2m/l2m.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

uint32_t l2m_crc32c(uint32_t crc, const void *buf, size_t len);

static l2m_fs *fs;

static void die(const char *what, int err)
{
	fprintf(stderr, "l2m: %s: %s\n", what, strerror(err < 0 ? -err : err));
	exit(1);
}

static uint32_t resolve(const char *path)
{
	uint32_t ino;
	int r = l2m_lookup_path(fs, path, &ino);
	if (r)
		die(path, r);
	return ino;
}

/* split "a/b/c" into parent inode and last name */
static uint32_t parent_of(const char *path, const char **name)
{
	const char *slash = strrchr(path, '/');
	if (!slash) {
		*name = path;
		return L2M_ROOT_INO;
	}
	*name = slash + 1;
	if (slash == path)
		return L2M_ROOT_INO;
	char *dir = strndup(path, (size_t)(slash - path));
	uint32_t ino = resolve(dir);
	free(dir);
	return ino;
}

static const char *type_name(uint32_t mode)
{
	switch (mode & 0170000) {
	case 0100000: return "file";
	case 0040000: return "dir";
	case 0120000: return "link";
	case 0020000: return "chr";
	case 0060000: return "blk";
	case 0010000: return "fifo";
	case 0140000: return "sock";
	}
	return "?";
}

struct ls_ctx {
	int count;
};

static int ls_cb(void *ctx, const char *name, size_t len, uint32_t ino, unsigned dtype, uint64_t next)
{
	(void)next;
	(void)dtype;
	struct ls_ctx *c = ctx;
	l2m_stat st;
	if (l2m_getattr(fs, ino, &st) == 0)
		printf("%-5s %06o %5u %5u %12" PRIu64 "  %.*s\n", type_name(st.mode), st.mode & 07777, st.uid, st.gid, st.size, (int)len, name);
	else
		printf("?     ?\t%.*s\n", (int)len, name);
	c->count++;
	return 0;
}

/* ---- tree: deterministic listing used by the test suite ---------------- */

struct names {
	char **v;
	uint32_t *ino;
	size_t n, cap;
};

static int collect_cb(void *ctx, const char *name, size_t len, uint32_t ino, unsigned dtype, uint64_t next)
{
	(void)dtype;
	(void)next;
	struct names *ns = ctx;
	if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
		return 0;
	if ((len == 10 && memcmp(name, "lost+found", 10) == 0))
		return 0;
	if (ns->n == ns->cap) {
		ns->cap = ns->cap ? ns->cap * 2 : 64;
		ns->v = realloc(ns->v, ns->cap * sizeof(char *));
		ns->ino = realloc(ns->ino, ns->cap * sizeof(uint32_t));
	}
	ns->v[ns->n] = strndup(name, len);
	ns->ino[ns->n] = ino;
	ns->n++;
	return 0;
}

static int cmpstr(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static uint32_t file_crc(uint32_t ino, uint64_t size)
{
	static uint8_t buf[1 << 20];
	uint32_t c = 0;
	for (uint64_t off = 0; off < size;) {
		int64_t n = l2m_read(fs, ino, off, buf, sizeof(buf));
		if (n <= 0)
			die("read", n ? (int)n : EIO);
		c = l2m_crc32c(c, buf, (size_t)n);
		off += (uint64_t)n;
	}
	return c;
}

static void tree(uint32_t dir, const char *prefix)
{
	struct names ns = {0};
	uint64_t cookie = 0;
	int r = l2m_readdir(fs, dir, &cookie, collect_cb, &ns);
	if (r)
		die("readdir", r);
	/* sort by name */
	for (size_t i = 0; i < ns.n; i++)
		for (size_t k = i + 1; k < ns.n; k++)
			if (strcmp(ns.v[k], ns.v[i]) < 0) {
				char *t = ns.v[i];
				ns.v[i] = ns.v[k];
				ns.v[k] = t;
				uint32_t ti = ns.ino[i];
				ns.ino[i] = ns.ino[k];
				ns.ino[k] = ti;
			}
	for (size_t i = 0; i < ns.n; i++) {
		char path[4096];
		snprintf(path, sizeof(path), "%s/%s", prefix, ns.v[i]);
		l2m_stat st;
		r = l2m_getattr(fs, ns.ino[i], &st);
		if (r)
			die(path, r);
		const char *t = type_name(st.mode);
		if ((st.mode & 0170000) == 0100000) {
			printf("%s\t%s\t%04o\t%" PRIu64 "\t%08x\n", path, t, st.mode & 07777, st.size, file_crc(ns.ino[i], st.size));
			static char xl[65536], xv[65536];
			int64_t xn = l2m_listxattr(fs, ns.ino[i], xl, sizeof(xl));
			if (xn < 0)
				die("listxattr", (int)xn);
			char *names[256];
			int nn = 0;
			for (int64_t k = 0; k < xn && nn < 256; k += (int64_t)strlen(xl + k) + 1)
				names[nn++] = xl + k;
			qsort(names, (size_t)nn, sizeof(char *), cmpstr);
			for (int k = 0; k < nn; k++) {
				int64_t vl = l2m_getxattr(fs, ns.ino[i], names[k], xv, sizeof(xv));
				if (vl < 0)
					die("getxattr", (int)vl);
				printf("%s\txattr\t%s\t%" PRId64 "\t%08x\n", path, names[k], vl, l2m_crc32c(0, xv, (size_t)vl));
			}
		} else if ((st.mode & 0170000) == 0120000) {
			char target[4096];
			r = l2m_readlink(fs, ns.ino[i], target, sizeof(target));
			if (r < 0)
				die(path, r);
			printf("%s\t%s\t%s\n", path, t, target);
		} else if ((st.mode & 0170000) == 0040000) {
			printf("%s\t%s\t%04o\n", path, t, st.mode & 07777);
			tree(ns.ino[i], path);
		} else {
			printf("%s\t%s\t%04o\n", path, t, st.mode & 07777);
		}
		free(ns.v[i]);
	}
	free(ns.v);
	free(ns.ino);
}

/* ---- zero holes: unwritten extents inside files ------------------------ */

struct zh {
	const char *path;
	uint64_t pieces;
	uint32_t bs;
};

static int zh_cb(void *ctx, uint64_t lblk, uint64_t pblk, uint32_t len)
{
	struct zh *z = ctx;
	if (z->pieces++ < 20)
		printf("%s\tblocks %" PRIu64 "-%" PRIu64 " (%u blocks, bytes %" PRIu64 "-%" PRIu64 ") at %" PRIu64 "\n", z->path, lblk,
		       lblk + len - 1, len, lblk * z->bs, (lblk + len) * z->bs - 1, pblk);
	return 0;
}

static void zero_holes(uint32_t dir, const char *prefix, uint32_t bs, uint64_t *files, uint64_t *blocks)
{
	struct names ns = {0};
	uint64_t cookie = 0;
	int r = l2m_readdir(fs, dir, &cookie, collect_cb, &ns);
	if (r)
		die("readdir", r);
	for (size_t i = 0; i < ns.n; i++) {
		char path[4096];
		snprintf(path, sizeof(path), "%s/%s", prefix, ns.v[i]);
		l2m_stat st;
		if (l2m_getattr(fs, ns.ino[i], &st) == 0) {
			if ((st.mode & 0170000) == 0100000) {
				struct zh z = {path, 0, bs};
				int64_t n = l2m_zero_holes(fs, ns.ino[i], zh_cb, &z);
				if (n < 0)
					fprintf(stderr, "%s: %s\n", path, strerror((int)-n));
				else if (n > 0) {
					(*files)++;
					*blocks += (uint64_t)n;
				}
			} else if ((st.mode & 0170000) == 0040000) {
				zero_holes(ns.ino[i], path, bs, files, blocks);
			}
		}
		free(ns.v[i]);
	}
	free(ns.v);
	free(ns.ino);
}

static void print_path(void *ctx, uint32_t ino, const char *path)
{
	(void)ctx;
	printf("%u\t%s\n", ino, path);
}

/* ---- copy in ------------------------------------------------------------ */

static void copy_xattrs(const char *local, uint32_t ino)
{
	static char names[65536], val[65536];
#ifdef __APPLE__
	ssize_t n = listxattr(local, names, sizeof(names), XATTR_NOFOLLOW);
#else
	ssize_t n = llistxattr(local, names, sizeof(names));
#endif
	for (ssize_t k = 0; k < n; k += (ssize_t)strlen(names + k) + 1) {
		const char *nm = names + k;
#ifdef __APPLE__
		ssize_t vl = getxattr(local, nm, val, sizeof(val), 0, XATTR_NOFOLLOW);
#else
		if (strncmp(nm, "user.", 5) != 0)
			continue;
		ssize_t vl = lgetxattr(local, nm, val, sizeof(val));
#endif
		if (vl < 0)
			continue;
		int r = l2m_setxattr(fs, ino, nm, val, (size_t)vl, 0);
		if (r)
			die(nm, r);
	}
}

static void put_file(const char *local, uint32_t dir, const char *name, mode_t mode)
{
	int fd = open(local, O_RDONLY);
	if (fd < 0)
		die(local, errno);
	uint32_t ino;
	int r = l2m_create(fs, dir, name, strlen(name), 0100000 | (mode & 07777), getuid(), getgid(), 0, &ino);
	if (r)
		die(name, r);
	static uint8_t buf[4 << 20];
	uint64_t off = 0;
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n < 0)
			die(local, errno);
		if (n == 0)
			break;
		int64_t w = l2m_write(fs, ino, off, buf, (size_t)n);
		if (w >= 0 && w < n) /* short write: the next call reports why */
			w = l2m_write(fs, ino, off + (uint64_t)w, buf + w, (size_t)(n - w));
		if (w < 0)
			die("write", (int)w);
		off += (uint64_t)n;
	}
	close(fd);
	copy_xattrs(local, ino);
}

static void copy_in(const char *local, uint32_t dir, const char *name)
{
	struct stat st;
	if (lstat(local, &st) != 0)
		die(local, errno);
	if (S_ISDIR(st.st_mode)) {
		uint32_t ino;
		int r = l2m_create(fs, dir, name, strlen(name), 0040000 | (st.st_mode & 07777), getuid(), getgid(), 0, &ino);
		if (r)
			die(name, r);
		DIR *d = opendir(local);
		if (!d)
			die(local, errno);
		struct dirent *de;
		while ((de = readdir(d))) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
				continue;
			char p[4096];
			snprintf(p, sizeof(p), "%s/%s", local, de->d_name);
			copy_in(p, ino, de->d_name);
		}
		closedir(d);
	} else if (S_ISLNK(st.st_mode)) {
		char t[4096];
		ssize_t n = readlink(local, t, sizeof(t) - 1);
		if (n < 0)
			die(local, errno);
		t[n] = 0;
		uint32_t ino;
		int r = l2m_symlink(fs, dir, name, strlen(name), t, getuid(), getgid(), &ino);
		if (r)
			die(name, r);
	} else if (S_ISREG(st.st_mode)) {
		put_file(local, dir, name, st.st_mode);
	}
}

/* ---- disk images with a partition table ---------------------------------
 * The file system of a whole-disk image (GPT) starts at its first partition
 * that holds ext2/3/4; reads and writes are shifted by that offset. */

struct shifted {
	l2m_dev inner;
	uint64_t off;
};

static int sh_read(void *c, uint64_t off, void *buf, size_t len)
{
	struct shifted *s = c;
	return s->inner.read(s->inner.ctx, off + s->off, buf, len);
}

static int sh_write(void *c, uint64_t off, const void *buf, size_t len)
{
	struct shifted *s = c;
	return s->inner.write(s->inner.ctx, off + s->off, buf, len);
}

static int sh_flush(void *c)
{
	struct shifted *s = c;
	return s->inner.flush(s->inner.ctx);
}

static int has_ext_magic(l2m_dev *d, uint64_t at)
{
	uint8_t m[2];
	return d->read(d->ctx, at + 1024 + 56, m, 2) == 0 && m[0] == 0x53 && m[1] == 0xEF;
}

/* offset of the first ext2/3/4 partition of a GPT disk, 0 if none */
static uint64_t gpt_ext_offset(l2m_dev *d)
{
	static uint8_t hdr[512], ent[512];
	for (uint32_t lba = 512; lba <= 4096; lba *= 8) {
		if (d->read(d->ctx, lba, hdr, sizeof(hdr)) || memcmp(hdr, "EFI PART", 8))
			continue;
		uint64_t first;
		uint32_t n, size;
		memcpy(&first, hdr + 72, 8);
		memcpy(&n, hdr + 80, 4);
		memcpy(&size, hdr + 84, 4);
		if (size < 128 || size > sizeof(ent))
			return 0;
		for (uint32_t i = 0; i < n && i < 256; i++) {
			uint64_t start;
			if (d->read(d->ctx, first * lba + (uint64_t)i * size, ent, size))
				return 0;
			memcpy(&start, ent + 32, 8);
			if (start && has_ext_magic(d, start * lba))
				return start * lba;
		}
	}
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: l2m [-w] [-s] <image|device> <command> [args]\n"
		"  -w  open read-write (default: strictly read-only)\n"
		"  -s  commit after every operation\n"
		"commands:\n"
		"  info | verify | statfs | label NAME\n"
		"  ls PATH | tree [PATH] | stat PATH | cat PATH | get PATH LOCAL\n"
		"  put LOCAL PATH | copyin LOCALDIR PATH | mkdir PATH | rm PATH | rmdir PATH\n"
		"  mv FROM TO | ln TARGET NEWPATH | symlink TARGET PATH | readlink PATH\n"
		"  truncate PATH SIZE | write PATH OFFSET TEXT | chmod MODE PATH\n"
		"  fallocate PATH SIZE (reserve space: the file grows, the new part reads as zeros)\n"
		"  setxattr PATH NAME VALUE | getxattr PATH NAME | listxattr PATH | rmxattr PATH NAME\n"
		"  bench PATH   (sequential read speed of a file)\n"
		"  extents PATH (all extents of a file) | zeroholes [PATH] (unwritten extents inside files)\n"
		"  paths INODE... (where these inodes are in the directory tree)\n");
	exit(2);
}

static double now_s(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

static void print_time(const char *label, int64_t t)
{
	char b[64] = "-";
	if (t) {
		time_t tt = (time_t)t;
		strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", localtime(&tt));
	}
	printf("%-22s %s\n", label, b);
}

static int run(const char *cmd, char **a, int na)
{
	int r, rc = 0;
	if (!strcmp(cmd, "info")) {
		l2m_info in;
		l2m_get_info(fs, &in);
		printf("%-22s %s\n", "Label", in.label);
		printf("%-22s %s\n", "UUID", in.uuid);
		printf("%-22s %s\n", "Features", in.features);
		printf("%-22s %s\n", "Read-only because", in.unsupported[0] ? in.unsupported : "-");
		printf("%-22s %u\n", "Block size", in.block_size);
		printf("%-22s %" PRIu64 " (%.1f GiB)\n", "Blocks", in.blocks_count, (double)in.blocks_count * in.block_size / 1073741824.0);
		printf("%-22s %" PRIu64 " (%.1f GiB)\n", "Free blocks", in.free_blocks, (double)in.free_blocks * in.block_size / 1073741824.0);
		printf("%-22s %u / %u free\n", "Inodes", in.inodes_count, in.free_inodes);
		printf("%-22s %u\n", "Groups", in.groups);
		printf("%-22s %s%s\n", "State", (in.state & 1) ? "clean" : "not clean", (in.state & 2) ? ", ERRORS" : "");
		printf("%-22s %u (max %d)\n", "Mount count", in.mount_count, in.max_mount_count);
		printf("%-22s %s\n", "Last mounted on", in.last_mounted);
		print_time("Created", in.mkfs_time);
		print_time("Last mount", in.mount_time);
		print_time("Last write", in.write_time);
		print_time("Last check", in.lastcheck_time);
		printf("%-22s %u\n", "Error count", in.error_count);
		printf("%-22s %s, %u blocks, features 0x%x\n", "Journal", in.has_journal ? "yes" : "no", in.journal_blocks, in.journal_features);
		printf("%-22s %s (%u transactions, %u blocks replayed)\n", "Journal recovery", in.journal_needed_recovery ? "needed" : "not needed",
		       in.journal_replayed_transactions, in.journal_replayed_blocks);
		printf("%-22s %s\n", "Metadata checksums", in.metadata_csum ? "yes" : "no");
		printf("%-22s %s\n", "Directory hash", in.hash_algo);
		printf("%-22s %s\n", "Mounted", in.readonly ? "read-only" : "read-write");
	} else if (!strcmp(cmd, "label") && na == 1) {
		r = l2m_set_label(fs, a[0]);
		if (r)
			die("label", r);
	} else if (!strcmp(cmd, "statfs")) {
		l2m_statfs st;
		l2m_statfs_get(fs, &st);
		printf("bsize %u blocks %" PRIu64 " bfree %" PRIu64 " bavail %" PRIu64 " files %" PRIu64 " ffree %" PRIu64 "\n", st.block_size,
		       st.blocks, st.bfree, st.bavail, st.files, st.ffree);
	} else if (!strcmp(cmd, "verify")) {
		static char rep[1 << 16];
		r = l2m_verify(fs, NULL, NULL, rep, sizeof(rep));
		fputs(rep, stdout);
		rc = r != 0;
	} else if (!strcmp(cmd, "ls")) {
		struct ls_ctx c = {0};
		uint64_t cookie = 0;
		r = l2m_readdir(fs, resolve(na ? a[0] : "/"), &cookie, ls_cb, &c);
		if (r)
			die("readdir", r);
	} else if (!strcmp(cmd, "tree")) {
		tree(resolve(na ? a[0] : "/"), "");
	} else if (!strcmp(cmd, "paths") && na >= 1) {
		uint32_t inos[64];
		int n = 0;
		for (int i = 0; i < na && n < 64; i++)
			inos[n++] = (uint32_t)strtoul(a[i], NULL, 10);
		r = l2m_find_paths(fs, inos, n, print_path, NULL);
		if (r)
			die("paths", r);
	} else if (!strcmp(cmd, "zeroholes")) {
		l2m_info in;
		l2m_get_info(fs, &in);
		uint64_t files = 0, blocks = 0;
		zero_holes(resolve(na ? a[0] : "/"), na && strcmp(a[0], "/") ? a[0] : "", in.block_size, &files, &blocks);
		printf("%" PRIu64 " file(s) with zero holes, %" PRIu64 " block(s) in total\n", files, blocks);
		rc = files != 0;
	} else if (!strcmp(cmd, "extents") && na == 1) {
		uint32_t ino = resolve(a[0]);
		l2m_stat st;
		l2m_info in;
		l2m_get_info(fs, &in);
		r = l2m_getattr(fs, ino, &st);
		if (r)
			die(a[0], r);
		uint64_t end = (st.size + in.block_size - 1) / in.block_size, n[3] = {0};
		l2m_extent ex[64];
		for (uint64_t lb = 0; lb < end;) {
			int k = l2m_map(fs, ino, lb, end - lb, 0, ex, 64);
			if (k < 0)
				die("map", k);
			if (k == 0)
				break;
			for (int i = 0; i < k; i++) {
				static const char *names[] = {"hole", "data", "unwritten"};
				printf("%10" PRIu64 " %8u  %s", ex[i].lblk, ex[i].len, names[ex[i].type % 3]);
				if (ex[i].pblk)
					printf(" at %" PRIu64, ex[i].pblk);
				printf("\n");
				n[ex[i].type % 3]++;
				lb = ex[i].lblk + ex[i].len;
			}
		}
		printf("%" PRIu64 " written, %" PRIu64 " unwritten, %" PRIu64 " hole(s)\n", n[1], n[2], n[0]);
	} else if (!strcmp(cmd, "stat") && na == 1) {
		l2m_stat st;
		r = l2m_getattr(fs, resolve(a[0]), &st);
		if (r)
			die(a[0], r);
		printf("inode %u type %s mode %04o links %u uid %u gid %u size %" PRIu64 " alloc %" PRIu64 " flags 0x%x\n", st.ino,
		       type_name(st.mode), st.mode & 07777, st.nlink, st.uid, st.gid, st.size, st.alloc, st.flags);
		printf("mtime %lld.%09u ctime %lld.%09u crtime %lld.%09u\n", (long long)st.mtime.sec, st.mtime.nsec, (long long)st.ctime.sec,
		       st.ctime.nsec, (long long)st.crtime.sec, st.crtime.nsec);
	} else if ((!strcmp(cmd, "cat") && na == 1) || (!strcmp(cmd, "get") && na == 2)) {
		uint32_t ino = resolve(a[0]);
		int out = 1;
		if (na == 2 && (out = open(a[1], O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
			die(a[1], errno);
		static uint8_t buf[4 << 20];
		for (uint64_t off = 0;;) {
			int64_t n = l2m_read(fs, ino, off, buf, sizeof(buf));
			if (n < 0)
				die("read", (int)n);
			if (n == 0)
				break;
			if (write(out, buf, (size_t)n) != n)
				die("write", errno);
			off += (uint64_t)n;
		}
		if (out != 1)
			close(out);
	} else if (!strcmp(cmd, "bench") && na == 1) {
		uint32_t ino = resolve(a[0]);
		static uint8_t buf[8 << 20];
		double t0 = now_s();
		uint64_t total = 0;
		for (;;) {
			int64_t n = l2m_read(fs, ino, total, buf, sizeof(buf));
			if (n < 0)
				die("read", (int)n);
			if (n == 0)
				break;
			total += (uint64_t)n;
		}
		double dt = now_s() - t0;
		printf("read %" PRIu64 " bytes in %.2f s = %.1f MB/s\n", total, dt, total / dt / 1e6);
	} else if (!strcmp(cmd, "put") && na == 2) {
		const char *name;
		uint32_t dir = parent_of(a[1], &name);
		struct stat st;
		if (stat(a[0], &st))
			die(a[0], errno);
		double t0 = now_s();
		put_file(a[0], dir, name, st.st_mode);
		r = l2m_sync(fs);
		if (r)
			die("sync", r);
		double dt = now_s() - t0;
		if (getenv("L2M_BENCH"))
			printf("wrote %lld bytes in %.2f s = %.1f MB/s\n", (long long)st.st_size, dt, st.st_size / dt / 1e6);
	} else if (!strcmp(cmd, "copyin") && na == 2) {
		const char *name;
		uint32_t dir = parent_of(a[1], &name);
		copy_in(a[0], dir, name);
	} else if (!strcmp(cmd, "mkdir") && na == 1) {
		const char *name;
		uint32_t dir = parent_of(a[0], &name), ino;
		r = l2m_create(fs, dir, name, strlen(name), 0040755, getuid(), getgid(), 0, &ino);
		if (r)
			die(a[0], r);
	} else if ((!strcmp(cmd, "rm") || !strcmp(cmd, "rmdir")) && na == 1) {
		const char *name;
		uint32_t dir = parent_of(a[0], &name), ino;
		r = l2m_lookup(fs, dir, name, strlen(name), &ino);
		if (r)
			die(a[0], r);
		int orphan = 0;
		r = l2m_unlink(fs, dir, name, strlen(name), !strcmp(cmd, "rmdir"), &orphan);
		if (r)
			die(a[0], r);
		if (orphan)
			r = l2m_release(fs, ino);
		if (r)
			die("release", r);
	} else if (!strcmp(cmd, "mv") && na == 2) {
		const char *sn, *dn;
		uint32_t sd = parent_of(a[0], &sn), dd = parent_of(a[1], &dn), repl;
		int orphan;
		r = l2m_rename(fs, sd, sn, strlen(sn), dd, dn, strlen(dn), 0, &repl, &orphan);
		if (r)
			die("rename", r);
		if (orphan)
			l2m_release(fs, repl);
	} else if (!strcmp(cmd, "ln") && na == 2) {
		const char *name;
		uint32_t dir = parent_of(a[1], &name);
		r = l2m_link(fs, resolve(a[0]), dir, name, strlen(name));
		if (r)
			die("link", r);
	} else if (!strcmp(cmd, "symlink") && na == 2) {
		const char *name;
		uint32_t dir = parent_of(a[1], &name), ino;
		r = l2m_symlink(fs, dir, name, strlen(name), a[0], getuid(), getgid(), &ino);
		if (r)
			die("symlink", r);
	} else if (!strcmp(cmd, "readlink") && na == 1) {
		char t[4096];
		r = l2m_readlink(fs, resolve(a[0]), t, sizeof(t));
		if (r < 0)
			die("readlink", r);
		puts(t);
	} else if (!strcmp(cmd, "fallocate") && na == 2) {
		uint32_t ino = resolve(a[0]);
		uint64_t size = strtoull(a[1], NULL, 0), got = 0;
		r = l2m_preallocate(fs, ino, 0, size, &got);
		l2m_stat st = {.size = size};
		if (!r)
			r = l2m_setattr(fs, ino, L2M_SET_SIZE, &st);
		if (r)
			die("fallocate", r);
		printf("%" PRIu64 " bytes reserved\n", got);
	} else if (!strcmp(cmd, "truncate") && na == 2) {
		r = l2m_truncate(fs, resolve(a[0]), strtoull(a[1], NULL, 0));
		if (r)
			die("truncate", r);
	} else if (!strcmp(cmd, "write") && na == 3) {
		int64_t w = l2m_write(fs, resolve(a[0]), strtoull(a[1], NULL, 0), a[2], strlen(a[2]));
		if (w < 0)
			die("write", (int)w);
	} else if (!strcmp(cmd, "chmod") && na == 2) {
		l2m_stat st = {0};
		st.mode = (uint32_t)strtoul(a[0], NULL, 8);
		r = l2m_setattr(fs, resolve(a[1]), L2M_SET_MODE, &st);
		if (r)
			die("chmod", r);
	} else if (!strcmp(cmd, "setxattr") && na == 3) {
		r = l2m_setxattr(fs, resolve(a[0]), a[1], a[2], strlen(a[2]), 0);
		if (r)
			die("setxattr", r);
	} else if (!strcmp(cmd, "rmxattr") && na == 2) {
		r = l2m_removexattr(fs, resolve(a[0]), a[1]);
		if (r)
			die("removexattr", r);
	} else if (!strcmp(cmd, "getxattr") && na == 2) {
		char v[65536];
		int64_t n = l2m_getxattr(fs, resolve(a[0]), a[1], v, sizeof(v));
		if (n < 0)
			die("getxattr", (int)n);
		fwrite(v, 1, (size_t)n, stdout);
		putchar('\n');
	} else if (!strcmp(cmd, "listxattr") && na == 1) {
		char v[65536];
		int64_t n = l2m_listxattr(fs, resolve(a[0]), v, sizeof(v));
		if (n < 0)
			die("listxattr", (int)n);
		for (int64_t i = 0; i < n; i += (int64_t)strlen(v + i) + 1)
			puts(v + i);
	} else {
		fprintf(stderr, "l2m: unknown command or wrong arguments: %s\n", cmd);
		return 2;
	}
	return rc;
}

int main(int argc, char **argv)
{
	int rw = 0, sync = 0, opt;
	while ((opt = getopt(argc, argv, "ws")) != -1) {
		if (opt == 'w')
			rw = 1;
		else if (opt == 's')
			sync = 1;
		else
			usage();
	}
	if (argc - optind < 2)
		usage();
	const char *img = argv[optind], *cmd = argv[optind + 1];
	char **a = argv + optind + 2;
	int na = argc - optind - 2;

	int fd = open(img, rw ? O_RDWR : O_RDONLY);
	if (fd < 0)
		die(img, errno);
	l2m_dev dev;
	int r = l2m_dev_open_fd(fd, rw, &dev);
	if (r)
		die("device", r);
	l2m_dev used = dev;
	struct shifted sh = {dev, 0};
	if (!has_ext_magic(&dev, 0) && (sh.off = gpt_ext_offset(&dev))) {
		used.ctx = &sh;
		used.read = sh_read;
		used.write = sh_write;
		used.flush = sh_flush;
		used.read_data = NULL;
		used.write_data = NULL;
		used.size = dev.size - sh.off;
	}
	char err[256];
	r = l2m_mount(&used, (rw ? 0 : L2M_MOUNT_RDONLY) | (sync ? L2M_MOUNT_SYNC : 0), &fs, err, sizeof(err));
	if (r) {
		fprintf(stderr, "l2m: mount failed: %s (%s)\n", err, strerror(-r));
		return 1;
	}
	if (rw && l2m_is_readonly(fs))
		fprintf(stderr, "l2m: warning: volume opened read-only (%s)\n", l2m_last_error(fs));

	int rc;
	if (!strcmp(cmd, "batch")) {
		/* one command per line, arguments separated by TAB */
		char line[16384];
		rc = 0;
		while (fgets(line, sizeof(line), stdin)) {
			line[strcspn(line, "\n")] = 0;
			if (!line[0] || line[0] == '#')
				continue;
			if (getenv("L2M_TRACE"))
				fprintf(stderr, "op: %.200s\n", line);
			char *v[16];
			int n = 0;
			for (char *t = strtok(line, "\t"); t && n < 16; t = strtok(NULL, "\t"))
				v[n++] = t;
			if (run(v[0], v + 1, n - 1))
				rc = 1;
		}
	} else {
		rc = run(cmd, a, na);
	}
	r = l2m_umount(fs);
	if (r) {
		fprintf(stderr, "l2m: unmount: %s\n", strerror(-r));
		rc = 1;
	}
	l2m_dev_close_fd(&dev);
	close(fd);
	return rc;
}
