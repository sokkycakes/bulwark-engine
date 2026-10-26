/* Bulwark engine - binary user info keys. ISC licence, see LICENSE. */
#include "infoblob.h"

void Blob_Set(infoblob_t *set, const char *key, const void *data, int len)
{
	infoblob_t *slot = NULL;
	for (int i = 0; i < MAX_INFOBLOBS && !slot; i++)
		if (set[i].key[0] && !Str_ICmp(set[i].key, key))
			slot = &set[i];
	for (int i = 0; i < MAX_INFOBLOBS && !slot; i++)
		if (!set[i].key[0])
			slot = &set[i];
	if (!slot)
		return;
	Mem_Free(slot->data);
	slot->data = NULL;
	slot->len = 0;
	slot->key[0] = 0;
	if (len <= 0 || len > MAX_INFOBLOB_SIZE)
		return;
	Str_Copy(slot->key, key, sizeof(slot->key));
	slot->data = Mem_Alloc((size_t)len);
	memcpy(slot->data, data, (size_t)len);
	slot->len = len;
}

const infoblob_t *Blob_Get(const infoblob_t *set, const char *key)
{
	for (int i = 0; i < MAX_INFOBLOBS; i++)
		if (set[i].key[0] && !Str_ICmp(set[i].key, key))
			return &set[i];
	return NULL;
}

void Blob_Clear(infoblob_t *set)
{
	for (int i = 0; i < MAX_INFOBLOBS; i++) {
		Mem_Free(set[i].data);
		memset(&set[i], 0, sizeof(set[i]));
	}
}
