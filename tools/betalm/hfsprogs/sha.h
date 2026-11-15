/* Minimal SHA-1 for newfs_hfs's volume UUID generation (replaces OpenSSL). */
#ifndef COMPAT_SHA_H
#define COMPAT_SHA_H
#include <stdint.h>
#include <string.h>
#include <stddef.h>
typedef struct { uint32_t h[5]; uint64_t n; unsigned char b[64]; size_t l; } SHA_CTX;
#define SHA_DIGEST_LENGTH 20
static inline uint32_t sha1_rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
static inline void sha1_block(SHA_CTX *c, const unsigned char *p) {
	uint32_t w[80], a, b, d, e, f, k, t, cc;
	for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
	for (int i = 16; i < 80; i++) w[i] = sha1_rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
	a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
	for (int i = 0; i < 80; i++) {
		if (i < 20) { f = (b & cc) | (~b & d); k = 0x5a827999; }
		else if (i < 40) { f = b ^ cc ^ d; k = 0x6ed9eba1; }
		else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8f1bbcdc; }
		else { f = b ^ cc ^ d; k = 0xca62c1d6; }
		t = sha1_rol(a, 5) + f + e + k + w[i]; e = d; d = cc; cc = sha1_rol(b, 30); b = a; a = t;
	}
	c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}
static inline int SHA1_Init(SHA_CTX *c) {
	c->h[0] = 0x67452301; c->h[1] = 0xefcdab89; c->h[2] = 0x98badcfe; c->h[3] = 0x10325476; c->h[4] = 0xc3d2e1f0;
	c->n = 0; c->l = 0; return 1;
}
static inline int SHA1_Update(SHA_CTX *c, const void *data, size_t len) {
	const unsigned char *p = data; c->n += len;
	while (len--) { c->b[c->l++] = *p++; if (c->l == 64) { sha1_block(c, c->b); c->l = 0; } }
	return 1;
}
static inline int SHA1_Final(unsigned char *md, SHA_CTX *c) {
	uint64_t bits = c->n * 8; unsigned char one = 0x80, zero = 0, len[8];
	SHA1_Update(c, &one, 1);
	while (c->l != 56) SHA1_Update(c, &zero, 1);
	for (int i = 0; i < 8; i++) len[i] = (unsigned char)(bits >> (56 - 8*i));
	SHA1_Update(c, len, 8);
	for (int i = 0; i < 5; i++) { md[4*i] = c->h[i] >> 24; md[4*i+1] = c->h[i] >> 16; md[4*i+2] = c->h[i] >> 8; md[4*i+3] = c->h[i]; }
	return 1;
}
#endif
