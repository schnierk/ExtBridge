/*
 * ExtBridge - CRC32C (Castagnoli) and CRC16 as used by ext4 and jbd2.
 *
 * Both are "raw" updates exactly like the Linux kernel's crc32c_le() and
 * crc16(): no pre- or post-inversion is done here, callers pass the seed.
 * Hardware instructions are used on arm64 (ARMv8 CRC) and x86-64 (SSE4.2)
 * when available, otherwise a slicing-by-8 table.
 *
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

#include <string.h>

static uint32_t crc32c_table[8][256];
static uint16_t crc16_table[256];
static int tables_ready;

static void init_tables(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1)));
		crc32c_table[0][i] = c;
		uint16_t d = (uint16_t)i;
		for (int k = 0; k < 8; k++)
			d = (uint16_t)((d >> 1) ^ (0xA001u & (0u - (d & 1))));
		crc16_table[i] = d;
	}
	for (uint32_t i = 0; i < 256; i++)
		for (int t = 1; t < 8; t++)
			crc32c_table[t][i] = (crc32c_table[t - 1][i] >> 8) ^ crc32c_table[0][crc32c_table[t - 1][i] & 0xFF];
	tables_ready = 1;
}

static uint32_t crc32c_sw(uint32_t crc, const uint8_t *p, size_t len)
{
	while (len && ((uintptr_t)p & 7)) {
		crc = crc32c_table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
		len--;
	}
	while (len >= 8) {
		uint64_t v;
		memcpy(&v, p, 8);
		v ^= crc;
		crc = crc32c_table[7][v & 0xFF] ^ crc32c_table[6][(v >> 8) & 0xFF] ^
		      crc32c_table[5][(v >> 16) & 0xFF] ^ crc32c_table[4][(v >> 24) & 0xFF] ^
		      crc32c_table[3][(v >> 32) & 0xFF] ^ crc32c_table[2][(v >> 40) & 0xFF] ^
		      crc32c_table[1][(v >> 48) & 0xFF] ^ crc32c_table[0][v >> 56];
		p += 8;
		len -= 8;
	}
	while (len--)
		crc = crc32c_table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return crc;
}

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
static uint32_t crc32c_hw(uint32_t crc, const uint8_t *p, size_t len)
{
	while (len && ((uintptr_t)p & 7)) {
		crc = __crc32cb(crc, *p++);
		len--;
	}
	while (len >= 8) {
		uint64_t v;
		memcpy(&v, p, 8);
		crc = __crc32cd(crc, v);
		p += 8;
		len -= 8;
	}
	while (len--)
		crc = __crc32cb(crc, *p++);
	return crc;
}
#define HAVE_HW_CRC 1
static int hw_ok(void) { return 1; }
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#include <nmmintrin.h>
__attribute__((target("sse4.2"))) static uint32_t crc32c_hw(uint32_t crc, const uint8_t *p, size_t len)
{
	uint64_t c = crc;
	while (len && ((uintptr_t)p & 7)) {
		c = _mm_crc32_u8((uint32_t)c, *p++);
		len--;
	}
	while (len >= 8) {
		uint64_t v;
		memcpy(&v, p, 8);
		c = _mm_crc32_u64(c, v);
		p += 8;
		len -= 8;
	}
	while (len--)
		c = _mm_crc32_u8((uint32_t)c, *p++);
	return (uint32_t)c;
}
#define HAVE_HW_CRC 1
static int hw_ok(void)
{
	static int state = -1;
	if (state < 0) {
		unsigned a, b, c, d;
		state = (__get_cpuid(1, &a, &b, &c, &d) && (c & bit_SSE4_2)) ? 1 : 0;
	}
	return state;
}
#endif

uint32_t l2m_crc32c(uint32_t crc, const void *buf, size_t len)
{
#ifdef HAVE_HW_CRC
	if (hw_ok() && !getenv_flag_no_hw_crc())
		return crc32c_hw(crc, buf, len);
#endif
	if (!tables_ready)
		init_tables();
	return crc32c_sw(crc, buf, len);
}

uint32_t l2m_crc32c_sw(uint32_t crc, const void *buf, size_t len)
{
	if (!tables_ready)
		init_tables();
	return crc32c_sw(crc, buf, len);
}

uint16_t l2m_crc16(uint16_t crc, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	if (!tables_ready)
		init_tables();
	while (len--)
		crc = (uint16_t)((crc >> 8) ^ crc16_table[(crc ^ *p++) & 0xFF]);
	return crc;
}
