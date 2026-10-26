/* Bulwark engine - network message buffers. ISC licence, see LICENSE. */
#ifndef BW_MSG_H
#define BW_MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A fixed-capacity byte buffer. Writes past the end set 'overflowed' and are
 * dropped; reads past the end set 'badread' and return zeroes. All values are
 * little endian. */
typedef struct {
	uint8_t *data;
	size_t maxsize;
	size_t cursize;
	size_t readpos;
	bool overflowed;
	bool badread;
	bool allowoverflow; /* overflow silently clears instead of being an error */
} msg_t;

void MSG_Init(msg_t *m, void *data, size_t size);
void MSG_Clear(msg_t *m);
void MSG_BeginReading(msg_t *m);
size_t MSG_Remaining(const msg_t *m);

void *MSG_GetSpace(msg_t *m, size_t len);
void MSG_WriteData(msg_t *m, const void *data, size_t len);
void MSG_WriteByte(msg_t *m, int c);
void MSG_WriteChar(msg_t *m, int c);
void MSG_WriteShort(msg_t *m, int c);
void MSG_WriteLong(msg_t *m, int32_t c);
void MSG_WriteInt64(msg_t *m, int64_t c);
void MSG_WriteFloat(msg_t *m, float f);
void MSG_WriteDouble(msg_t *m, double d);
void MSG_WriteString(msg_t *m, const char *s);
void MSG_WriteCoord(msg_t *m, float f);  /* full float */
void MSG_WriteAngle(msg_t *m, float f);  /* 16 bits per 360 degrees */
void MSG_WriteAngle8(msg_t *m, float f); /* 8 bits per 360 degrees */
void MSG_WriteVarUInt(msg_t *m, uint32_t v);
void MSG_WriteEntity(msg_t *m, int e); /* variable length: 1 to 3 bytes */

bool MSG_ReadData(msg_t *m, void *out, size_t len);
int MSG_ReadByte(msg_t *m);
int MSG_ReadChar(msg_t *m);
int MSG_ReadShort(msg_t *m);
int MSG_ReadUShort(msg_t *m);
int32_t MSG_ReadLong(msg_t *m);
int64_t MSG_ReadInt64(msg_t *m);
float MSG_ReadFloat(msg_t *m);
double MSG_ReadDouble(msg_t *m);
/* returns a pointer to a static buffer (rotating among 4) */
const char *MSG_ReadString(msg_t *m);
float MSG_ReadCoord(msg_t *m);
float MSG_ReadAngle(msg_t *m);
float MSG_ReadAngle8(msg_t *m);
uint32_t MSG_ReadVarUInt(msg_t *m);
int MSG_ReadEntity(msg_t *m);

#endif
