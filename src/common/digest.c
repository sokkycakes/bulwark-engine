/* Bulwark engine - message digests, written from RFC 1321 and FIPS 180-4.
 * ISC licence, see LICENSE. */
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "digest.h"

uint16_t Digest_CRC16(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint16_t crc = 0xffff;
	while (len--) {
		crc ^= (uint16_t)(*p++ << 8);
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
	}
	return crc;
}

static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

/* Pads the message the way MD5 and SHA-1/2 share: 0x80, zeros, then the bit
 * length as 64 bits (little endian for MD5, big endian for SHA). Calls fn
 * once per 64-byte block. */
static void md_blocks(const uint8_t *data, size_t len, bool bigendian, void (*fn)(void *st, const uint8_t *blk), void *st)
{
	uint8_t tail[128];
	size_t full = len & ~(size_t)63, rest = len - full, taillen;
	uint64_t bits = (uint64_t)len * 8;
	for (size_t i = 0; i < full; i += 64)
		fn(st, data + i);
	memset(tail, 0, sizeof(tail));
	memcpy(tail, data + full, rest);
	tail[rest] = 0x80;
	taillen = rest + 1 + 8 <= 64 ? 64 : 128;
	for (int i = 0; i < 8; i++)
		tail[bigendian ? taillen - 1 - i : taillen - 8 + i] = (uint8_t)(bits >> (8 * i));
	for (size_t i = 0; i < taillen; i += 64)
		fn(st, tail + i);
}

/* --- MD5 --- */
static const uint32_t md5_k[64] = {
	0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
	0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
	0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
	0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
	0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
	0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
	0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
	0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
static const uint8_t md5_r[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                                  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4,  11, 16, 23,
                                  4,  11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void md5_block(void *st, const uint8_t *blk)
{
	uint32_t *h = st, m[16], a = h[0], b = h[1], c = h[2], d = h[3];
	for (int i = 0; i < 16; i++)
		m[i] = blk[i * 4] | blk[i * 4 + 1] << 8 | blk[i * 4 + 2] << 16 | (uint32_t)blk[i * 4 + 3] << 24;
	for (int i = 0; i < 64; i++) {
		uint32_t f, t;
		int g;
		if (i < 16)
			f = (b & c) | (~b & d), g = i;
		else if (i < 32)
			f = (d & b) | (~d & c), g = (5 * i + 1) & 15;
		else if (i < 48)
			f = b ^ c ^ d, g = (3 * i + 5) & 15;
		else
			f = c ^ (b | ~d), g = (7 * i) & 15;
		t = d;
		d = c;
		c = b;
		b = b + rol(a + f + md5_k[i] + m[g], md5_r[i]);
		a = t;
	}
	h[0] += a, h[1] += b, h[2] += c, h[3] += d;
}

void Digest_MD5(const void *data, size_t len, uint8_t out[16])
{
	uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
	md_blocks(data, len, false, md5_block, h);
	for (int i = 0; i < 16; i++)
		out[i] = (uint8_t)(h[i / 4] >> (8 * (i & 3)));
}

/* --- SHA-1 --- */
static void sha1_block(void *st, const uint8_t *blk)
{
	uint32_t *h = st, w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)blk[i * 4] << 24 | blk[i * 4 + 1] << 16 | blk[i * 4 + 2] << 8 | blk[i * 4 + 3];
	for (int i = 16; i < 80; i++)
		w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	for (int i = 0; i < 80; i++) {
		uint32_t f, k, t;
		if (i < 20)
			f = (b & c) | (~b & d), k = 0x5a827999;
		else if (i < 40)
			f = b ^ c ^ d, k = 0x6ed9eba1;
		else if (i < 60)
			f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdc;
		else
			f = b ^ c ^ d, k = 0xca62c1d6;
		t = rol(a, 5) + f + e + k + w[i];
		e = d, d = c, c = rol(b, 30), b = a, a = t;
	}
	h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
}

void Digest_SHA1(const void *data, size_t len, uint8_t out[20])
{
	uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
	md_blocks(data, len, true, sha1_block, h);
	for (int i = 0; i < 20; i++)
		out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i & 3)));
}

/* --- SHA-256 --- */
static const uint32_t sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void sha256_block(void *st, const uint8_t *blk)
{
	uint32_t *h = st, w[64], v[8];
	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)blk[i * 4] << 24 | blk[i * 4 + 1] << 16 | blk[i * 4 + 2] << 8 | blk[i * 4 + 3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	memcpy(v, h, sizeof(v));
	for (int i = 0; i < 64; i++) {
		uint32_t s1 = ror(v[4], 6) ^ ror(v[4], 11) ^ ror(v[4], 25);
		uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
		uint32_t t1 = v[7] + s1 + ch + sha256_k[i] + w[i];
		uint32_t s0 = ror(v[0], 2) ^ ror(v[0], 13) ^ ror(v[0], 22);
		uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
		memmove(v + 1, v, 7 * sizeof(uint32_t));
		v[4] += t1;
		v[0] = t1 + s0 + maj;
	}
	for (int i = 0; i < 8; i++)
		h[i] += v[i];
}

void Digest_SHA256(const void *data, size_t len, uint8_t out[32])
{
	uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	md_blocks(data, len, true, sha256_block, h);
	for (int i = 0; i < 32; i++)
		out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i & 3)));
}

bool Digest_Hex(const char *algo, const void *data, size_t len, char *out, size_t outsize)
{
	uint8_t buf[32];
	size_t n;
	if (!Str_ICmp(algo, "CRC16")) {
		uint16_t c = Digest_CRC16(data, len);
		buf[0] = (uint8_t)(c >> 8), buf[1] = (uint8_t)c, n = 2;
	} else if (!Str_ICmp(algo, "MD5"))
		Digest_MD5(data, len, buf), n = 16;
	else if (!Str_ICmp(algo, "SHA1"))
		Digest_SHA1(data, len, buf), n = 20;
	else if (!Str_ICmp(algo, "SHA256"))
		Digest_SHA256(data, len, buf), n = 32;
	else
		return false;
	if (outsize < n * 2 + 1)
		return false;
	for (size_t i = 0; i < n; i++)
		snprintf(out + i * 2, 3, "%02x", buf[i]);
	return true;
}
