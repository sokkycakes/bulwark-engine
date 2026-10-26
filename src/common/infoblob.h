/* Bulwark engine - binary user info keys ("info blobs"), such as a
 * player's spray image. ISC licence, see LICENSE. */
#ifndef BW_INFOBLOB_H
#define BW_INFOBLOB_H

#include "common.h"

#define MAX_INFOBLOBS 8
#define MAX_INFOBLOB_SIZE 65536

typedef struct {
	char key[64];
	uint8_t *data;
	int len;
} infoblob_t;

/* replaces or adds the key; an empty blob removes it */
void Blob_Set(infoblob_t *set, const char *key, const void *data, int len);
const infoblob_t *Blob_Get(const infoblob_t *set, const char *key);
void Blob_Clear(infoblob_t *set);

#endif
