/* Bulwark engine - Source BSP container: lump directory, LZMA lumps, game
 * lumps and the static prop lump. See docs/spec/vbsp.md.
 * ISC licence, see LICENSE. */
#include "vbsp.h"
#include "../thirdparty/lzma/LzmaDec.h"

static int32_t rd32(const uint8_t *p) { return (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }
static uint16_t rdu16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static float rdf(const uint8_t *p)
{
	int32_t i = rd32(p);
	float f;
	memcpy(&f, &i, 4);
	return f;
}

bool VBSP_Open(vbsp_t *v, const uint8_t *data, size_t size)
{
	memset(v, 0, sizeof(*v));
	if (size < 8 + VL_NUMLUMPS * 16 + 4 || memcmp(data, "VBSP", 4))
		return false;
	v->file = data;
	v->size = size;
	v->version = rd32(data + 4);
	for (int i = 0; i < VL_NUMLUMPS; i++) {
		const uint8_t *p = data + 8 + i * 16;
		v->lumps[i].ofs = rd32(p);
		v->lumps[i].len = rd32(p + 4);
		v->lumps[i].version = rd32(p + 8);
		v->lumps[i].fourcc = rd32(p + 12);
		if (v->lumps[i].ofs < 0 || v->lumps[i].len < 0 || (size_t)v->lumps[i].ofs + v->lumps[i].len > size)
			v->lumps[i].ofs = v->lumps[i].len = 0;
	}
	return true;
}

static void *lz_alloc(ISzAllocPtr p, size_t size)
{
	(void)p;
	return Mem_Alloc(size ? size : 1);
}
static void lz_free(ISzAllocPtr p, void *a)
{
	(void)p;
	Mem_Free(a);
}

uint8_t *VBSP_Unlzma(const uint8_t *src, size_t srclen, size_t *outlen)
{
	static const ISzAlloc alloc = {lz_alloc, lz_free};
	SizeT dlen, slen;
	ELzmaStatus status;
	uint8_t *out;
	uint32_t actual, packed;
	*outlen = 0;
	if (srclen < 17 || memcmp(src, "LZMA", 4))
		return NULL;
	actual = (uint32_t)rd32(src + 4);
	packed = (uint32_t)rd32(src + 8);
	if (packed > srclen - 17 || actual > (1u << 30))
		return NULL;
	out = Mem_Alloc(actual + 1);
	dlen = actual;
	slen = packed;
	if (LzmaDecode(out, &dlen, src + 17, &slen, src + 12, LZMA_PROPS_SIZE, LZMA_FINISH_ANY, &status, &alloc) != SZ_OK ||
	    dlen != actual) {
		Mem_Free(out);
		return NULL;
	}
	*outlen = actual;
	return out;
}

/* a copy of size bytes at data, decompressed when it starts with "LZMA" */
static uint8_t *copy_block(const uint8_t *data, size_t size, size_t *len)
{
	uint8_t *out;
	if (size >= 17 && !memcmp(data, "LZMA", 4))
		return VBSP_Unlzma(data, size, len);
	out = Mem_Alloc(size + 1);
	memcpy(out, data, size);
	*len = size;
	return out;
}

uint8_t *VBSP_Lump(const vbsp_t *v, int lump, size_t *len)
{
	*len = 0;
	if (lump < 0 || lump >= VL_NUMLUMPS || !v->lumps[lump].len)
		return NULL;
	return copy_block(v->file + v->lumps[lump].ofs, (size_t)v->lumps[lump].len, len);
}

/* game lump directory: count, then 16-byte entries of id, flags (1 =
 * compressed), version, file offset and length. A compressed entry's
 * length is its decompressed size; its stored size runs to the next
 * entry's offset (the directory ends with an empty entry for that) */
uint8_t *VBSP_GameLump(const vbsp_t *v, const char id[4], int *version, size_t *len)
{
	const uint8_t *g = v->file + v->lumps[VL_GAME_LUMP].ofs;
	int glen = v->lumps[VL_GAME_LUMP].len, n;
	*len = 0;
	if (glen < 4)
		return NULL;
	n = rd32(g);
	if (n < 0 || 4 + (int64_t)n * 16 > glen)
		return NULL;
	for (int i = 0; i < n; i++) {
		const uint8_t *e = g + 4 + i * 16;
		int flags = rdu16(e + 4), ofs = rd32(e + 8), elen = rd32(e + 12), end;
		/* the id is a little-endian four-character code: "sprp" is stored "prps" */
		if (e[0] != id[3] || e[1] != id[2] || e[2] != id[1] || e[3] != id[0])
			continue;
		*version = rdu16(e + 6);
		if (ofs <= 0 || (size_t)ofs >= v->size)
			return NULL;
		if (flags & 1) {
			end = i + 1 < n ? rd32(e + 16 + 8) : 0;
			if (end <= ofs || (size_t)end > v->size)
				end = v->lumps[VL_GAME_LUMP].ofs + glen;
			return VBSP_Unlzma(v->file + ofs, (size_t)(end - ofs), len);
		}
		if (elen < 0 || (size_t)ofs + elen > v->size)
			return NULL;
		return copy_block(v->file + ofs, (size_t)elen, len);
	}
	return NULL;
}

/* "sprp": a dictionary of 128-byte model names, a leaf list of ushorts,
 * then the props. Every version from 4 starts each record with origin,
 * angles, the dictionary index, first leaf, leaf count and the solid type
 * at byte 30; later versions only add fields after it. */
bool VBSP_StaticProps(const vbsp_t *v, vbsp_staticprops_t *out)
{
	size_t len, p = 0;
	int ver = 0, nleafs, recsize;
	uint8_t *d = VBSP_GameLump(v, "sprp", &ver, &len);
	memset(out, 0, sizeof(*out));
	if (!d)
		return false;
	out->version = ver;
	if (len < 4)
		goto fail;
	out->numnames = rd32(d);
	p = 4;
	if (out->numnames < 0 || p + (size_t)out->numnames * 128 + 4 > len)
		goto fail;
	out->names = Mem_Alloc(sizeof(*out->names) * (out->numnames + 1));
	for (int i = 0; i < out->numnames; i++) {
		memcpy(out->names[i], d + p, 127);
		p += 128;
	}
	nleafs = rd32(d + p);
	p += 4;
	if (nleafs < 0 || p + (size_t)nleafs * 2 + 4 > len)
		goto fail;
	p += (size_t)nleafs * 2;
	out->numprops = rd32(d + p);
	p += 4;
	if (out->numprops <= 0 || out->numprops > 65536)
		goto done;
	recsize = (int)((len - p) / out->numprops);
	if (recsize < 32)
		goto fail;
	out->props = Mem_Alloc(sizeof(vbsp_staticprop_t) * out->numprops);
	for (int i = 0; i < out->numprops; i++) {
		const uint8_t *r = d + p + (size_t)i * recsize;
		vbsp_staticprop_t *sp = &out->props[i];
		for (int k = 0; k < 3; k++) {
			sp->origin[k] = rdf(r + k * 4);
			sp->angles[k] = rdf(r + 12 + k * 4);
		}
		sp->model = rdu16(r + 24);
		sp->firstleaf = rdu16(r + 26);
		sp->numleafs = rdu16(r + 28);
		sp->solid = r[30];
		if (sp->model >= out->numnames)
			sp->model = -1;
	}
done:
	Mem_Free(d);
	return true;
fail:
	Mem_Free(d);
	VBSP_FreeStaticProps(out);
	return false;
}

void VBSP_FreeStaticProps(vbsp_staticprops_t *sp)
{
	Mem_Free(sp->names);
	Mem_Free(sp->props);
	memset(sp, 0, sizeof(*sp));
}
