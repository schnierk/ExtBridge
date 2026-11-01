/* prints l2m_dirhash for argv names: hashtest VERSION SEED(uuid) name... */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
int l2m_dirhash(int version, const uint32_t seed[4], const char *name, int len, uint32_t *hash, uint32_t *minor);
int main(int argc, char **argv)
{
	int v = atoi(argv[1]);
	uint32_t seed[4];
	unsigned char u[16];
	const char *s = argv[2];
	for (int i = 0, k = 0; s[k] && i < 16; k++) {
		if (s[k] == '-') continue;
		sscanf(s + k, "%2hhx", &u[i++]);
		k++;
	}
	memcpy(seed, u, 16);
	for (int i = 3; i < argc; i++) {
		uint32_t h, m;
		l2m_dirhash(v, seed, argv[i], (int)strlen(argv[i]), &h, &m);
		printf("%08x %08x\n", h, m);
	}
	return 0;
}
