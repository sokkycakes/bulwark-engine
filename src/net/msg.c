/* Bulwark engine - network message buffers. ISC licence, see LICENSE. */
#include <math.h>
#include <string.h>

#include "../common/common.h"
#include "msg.h"

void MSG_Init(msg_t *m, void *data, size_t size)
{
	memset(m, 0, sizeof(*m));
	m->data = data;
	m->maxsize = size;
}

void MSG_Clear(msg_t *m)
{
	m->cursize = 0;
	m->readpos = 0;
	m->overflowed = false;
	m->badread = false;
}

void MSG_BeginReading(msg_t *m)
{
	m->readpos = 0;
	m->badread = false;
}

size_t MSG_Remaining(const msg_t *m) { return m->readpos < m->cursize ? m->cursize - m->readpos : 0; }

void *MSG_GetSpace(msg_t *m, size_t len)
{
	void *p;
	if (m->cursize + len > m->maxsize) {
		if (!m->allowoverflow && !m->overflowed)
			Con_DPrintf("message overflowed (%zu + %zu > %zu)\n", m->cursize, len, m->maxsize);
		m->overflowed = true;
		if (len > m->maxsize)
			return NULL;
		m->cursize = 0;
	}
	p = m->data + m->cursize;
	m->cursize += len;
	return p;
}

void MSG_WriteData(msg_t *m, const void *data, size_t len)
{
	void *p = MSG_GetSpace(m, len);
	if (p && len)
		memcpy(p, data, len);
}

static void write_le(msg_t *m, uint64_t v, int bytes)
{
	uint8_t *p = MSG_GetSpace(m, (size_t)bytes);
	if (!p)
		return;
	for (int i = 0; i < bytes; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

void MSG_WriteByte(msg_t *m, int c) { write_le(m, (uint8_t)c, 1); }
void MSG_WriteChar(msg_t *m, int c) { write_le(m, (uint8_t)(int8_t)c, 1); }
void MSG_WriteShort(msg_t *m, int c) { write_le(m, (uint16_t)c, 2); }
void MSG_WriteLong(msg_t *m, int32_t c) { write_le(m, (uint32_t)c, 4); }
void MSG_WriteInt64(msg_t *m, int64_t c) { write_le(m, (uint64_t)c, 8); }

void MSG_WriteFloat(msg_t *m, float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	write_le(m, u, 4);
}

void MSG_WriteDouble(msg_t *m, double d)
{
	uint64_t u;
	memcpy(&u, &d, 8);
	write_le(m, u, 8);
}

void MSG_WriteString(msg_t *m, const char *s)
{
	MSG_WriteData(m, s ? s : "", s ? strlen(s) + 1 : 1);
}

void MSG_WriteCoord(msg_t *m, float f) { MSG_WriteFloat(m, f); }
void MSG_WriteAngle(msg_t *m, float f) { MSG_WriteShort(m, (int)lrintf(f * 65536.0f / 360.0f) & 0xffff); }
void MSG_WriteAngle8(msg_t *m, float f) { MSG_WriteByte(m, (int)lrintf(f * 256.0f / 360.0f) & 0xff); }

void MSG_WriteVarUInt(msg_t *m, uint32_t v)
{
	while (v >= 0x80) {
		MSG_WriteByte(m, (int)(v & 0x7f) | 0x80);
		v >>= 7;
	}
	MSG_WriteByte(m, (int)v);
}

void MSG_WriteEntity(msg_t *m, int e) { MSG_WriteVarUInt(m, (uint32_t)e); }

bool MSG_ReadData(msg_t *m, void *out, size_t len)
{
	if (m->readpos + len > m->cursize) {
		m->badread = true;
		m->readpos = m->cursize;
		if (out)
			memset(out, 0, len);
		return false;
	}
	if (out)
		memcpy(out, m->data + m->readpos, len);
	m->readpos += len;
	return true;
}

static uint64_t read_le(msg_t *m, int bytes)
{
	uint8_t b[8];
	uint64_t v = 0;
	if (!MSG_ReadData(m, b, (size_t)bytes))
		return 0;
	for (int i = 0; i < bytes; i++)
		v |= (uint64_t)b[i] << (8 * i);
	return v;
}

int MSG_ReadByte(msg_t *m)
{
	if (m->readpos >= m->cursize) {
		m->badread = true;
		return -1;
	}
	return m->data[m->readpos++];
}

int MSG_ReadChar(msg_t *m) { return (int8_t)read_le(m, 1); }
int MSG_ReadShort(msg_t *m) { return (int16_t)read_le(m, 2); }
int MSG_ReadUShort(msg_t *m) { return (uint16_t)read_le(m, 2); }
int32_t MSG_ReadLong(msg_t *m) { return (int32_t)read_le(m, 4); }
int64_t MSG_ReadInt64(msg_t *m) { return (int64_t)read_le(m, 8); }

float MSG_ReadFloat(msg_t *m)
{
	uint32_t u = (uint32_t)read_le(m, 4);
	float f;
	memcpy(&f, &u, 4);
	return f;
}

double MSG_ReadDouble(msg_t *m)
{
	uint64_t u = read_le(m, 8);
	double d;
	memcpy(&d, &u, 8);
	return d;
}

const char *MSG_ReadString(msg_t *m)
{
	static char bufs[4][8192];
	static int which;
	char *out = bufs[which++ & 3];
	size_t n = 0;
	for (;;) {
		int c = MSG_ReadByte(m);
		if (c <= 0)
			break;
		if (n < sizeof(bufs[0]) - 1)
			out[n++] = (char)c;
	}
	out[n] = 0;
	return out;
}

float MSG_ReadCoord(msg_t *m) { return MSG_ReadFloat(m); }
float MSG_ReadAngle(msg_t *m) { return MSG_ReadUShort(m) * (360.0f / 65536.0f); }
float MSG_ReadAngle8(msg_t *m) { return (MSG_ReadByte(m) & 0xff) * (360.0f / 256.0f); }

uint32_t MSG_ReadVarUInt(msg_t *m)
{
	uint32_t v = 0;
	for (int shift = 0; shift < 35; shift += 7) {
		int c = MSG_ReadByte(m);
		if (c < 0)
			return 0;
		v |= (uint32_t)(c & 0x7f) << shift;
		if (!(c & 0x80))
			break;
	}
	return v;
}

int MSG_ReadEntity(msg_t *m) { return (int)MSG_ReadVarUInt(m); }
