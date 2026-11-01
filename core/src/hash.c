/*
 * ExtBridge - ext4 directory index (htree) name hashes.
 *
 * Re-implementation of the hash functions described in the ext4 disk
 * layout documentation (legacy "dx_hack", half MD4 and TEA, in signed and
 * unsigned char variants). Results are checked against debugfs in the test
 * suite (tests/test_hash.sh).
 *
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

#include <string.h>

#define HTREE_EOF_32BIT 0x7fffffffu

static uint32_t rol32(uint32_t w, unsigned s) { return (w << s) | (w >> (32 - s)); }

static void tea_transform(uint32_t buf[4], const uint32_t in[4])
{
	uint32_t sum = 0, b0 = buf[0], b1 = buf[1];
	uint32_t a = in[0], b = in[1], c = in[2], d = in[3];
	for (int n = 0; n < 16; n++) {
		sum += 0x9E3779B9u;
		b0 += ((b1 << 4) + a) ^ (b1 + sum) ^ ((b1 >> 5) + b);
		b1 += ((b0 << 4) + c) ^ (b0 + sum) ^ ((b0 >> 5) + d);
	}
	buf[0] += b0;
	buf[1] += b1;
}

#define F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define G(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define ROUND(f, a, b, c, d, x, s) (a += f(b, c, d) + (x), a = rol32(a, s))
#define K1 0u
#define K2 013240474631u
#define K3 015666365641u

static void half_md4_transform(uint32_t buf[4], const uint32_t in[8])
{
	uint32_t a = buf[0], b = buf[1], c = buf[2], d = buf[3];

	ROUND(F, a, b, c, d, in[0] + K1, 3);
	ROUND(F, d, a, b, c, in[1] + K1, 7);
	ROUND(F, c, d, a, b, in[2] + K1, 11);
	ROUND(F, b, c, d, a, in[3] + K1, 19);
	ROUND(F, a, b, c, d, in[4] + K1, 3);
	ROUND(F, d, a, b, c, in[5] + K1, 7);
	ROUND(F, c, d, a, b, in[6] + K1, 11);
	ROUND(F, b, c, d, a, in[7] + K1, 19);

	ROUND(G, a, b, c, d, in[1] + K2, 3);
	ROUND(G, d, a, b, c, in[3] + K2, 5);
	ROUND(G, c, d, a, b, in[5] + K2, 9);
	ROUND(G, b, c, d, a, in[7] + K2, 13);
	ROUND(G, a, b, c, d, in[0] + K2, 3);
	ROUND(G, d, a, b, c, in[2] + K2, 5);
	ROUND(G, c, d, a, b, in[4] + K2, 9);
	ROUND(G, b, c, d, a, in[6] + K2, 13);

	ROUND(H, a, b, c, d, in[3] + K3, 3);
	ROUND(H, d, a, b, c, in[7] + K3, 9);
	ROUND(H, c, d, a, b, in[2] + K3, 11);
	ROUND(H, b, c, d, a, in[6] + K3, 15);
	ROUND(H, a, b, c, d, in[1] + K3, 3);
	ROUND(H, d, a, b, c, in[5] + K3, 9);
	ROUND(H, c, d, a, b, in[0] + K3, 11);
	ROUND(H, b, c, d, a, in[4] + K3, 15);

	buf[0] += a;
	buf[1] += b;
	buf[2] += c;
	buf[3] += d;
}

static uint32_t dx_hack_hash(const char *name, int len, int is_unsigned)
{
	uint32_t hash, hash0 = 0x12a3fe2d, hash1 = 0x37abe8f9;
	for (int i = 0; i < len; i++) {
		int c = is_unsigned ? (int)(unsigned char)name[i] : (int)(signed char)name[i];
		hash = hash1 + (hash0 ^ (uint32_t)(c * 7152373));
		if (hash & 0x80000000u)
			hash -= 0x7fffffff;
		hash1 = hash0;
		hash0 = hash;
	}
	return hash0 << 1;
}

static void str2hashbuf(const char *msg, int len, uint32_t *buf, int num, int is_unsigned)
{
	uint32_t pad, val;
	pad = (uint32_t)len | ((uint32_t)len << 8);
	pad |= pad << 16;
	val = pad;
	if (len > num * 4)
		len = num * 4;
	for (int i = 0; i < len; i++) {
		int c = is_unsigned ? (int)(unsigned char)msg[i] : (int)(signed char)msg[i];
		val = (uint32_t)c + (val << 8);
		if ((i % 4) == 3) {
			*buf++ = val;
			val = pad;
			num--;
		}
	}
	if (--num >= 0)
		*buf++ = val;
	while (--num >= 0)
		*buf++ = pad;
}

int l2m_dirhash(int version, const uint32_t seed[4], const char *name, int len, uint32_t *hash_out, uint32_t *minor_out)
{
	uint32_t buf[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
	uint32_t in[8], hash = 0, minor = 0;
	const char *p;
	int is_unsigned = 0, l;

	if (seed && (seed[0] | seed[1] | seed[2] | seed[3]))
		memcpy(buf, seed, sizeof(buf));

	switch (version) {
	case EXT4_HASH_LEGACY_UNSIGNED:
		is_unsigned = 1;
		/* fallthrough */
	case EXT4_HASH_LEGACY:
		hash = dx_hack_hash(name, len, is_unsigned);
		break;
	case EXT4_HASH_HALF_MD4_UNSIGNED:
		is_unsigned = 1;
		/* fallthrough */
	case EXT4_HASH_HALF_MD4:
		for (p = name, l = len; l > 0; l -= 32, p += 32) {
			str2hashbuf(p, l, in, 8, is_unsigned);
			half_md4_transform(buf, in);
		}
		minor = buf[2];
		hash = buf[1];
		break;
	case EXT4_HASH_TEA_UNSIGNED:
		is_unsigned = 1;
		/* fallthrough */
	case EXT4_HASH_TEA:
		for (p = name, l = len; l > 0; l -= 16, p += 16) {
			str2hashbuf(p, l, in, 4, is_unsigned);
			tea_transform(buf, in);
		}
		hash = buf[0];
		minor = buf[1];
		break;
	default:
		return -EOPNOTSUPP; /* siphash (casefold) is not supported */
	}
	hash &= ~1u;
	if (hash == (HTREE_EOF_32BIT << 1))
		hash = (HTREE_EOF_32BIT - 1) << 1;
	if (hash_out)
		*hash_out = hash;
	if (minor_out)
		*minor_out = minor;
	return 0;
}
