/* Bulwark engine - message digests. ISC licence, see LICENSE. */
#ifndef BW_DIGEST_H
#define BW_DIGEST_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xffff, no reflection, no final xor */
uint16_t Digest_CRC16(const void *data, size_t len);
void Digest_MD5(const void *data, size_t len, uint8_t out[16]);
void Digest_SHA1(const void *data, size_t len, uint8_t out[20]);
void Digest_SHA256(const void *data, size_t len, uint8_t out[32]);

/* Hashes with the named algorithm ("CRC16", "MD5", "SHA1", "SHA256", case
 * insensitive) and writes lowercase hex. Returns false for unknown names. */
bool Digest_Hex(const char *algo, const void *data, size_t len, char *out, size_t outsize);

#endif
