/* Bulwark engine - BSP file tools: BSPX extension lumps (listing, stripping,
 * adding .lit/.lux files, writing renderer data), rewriting a BSP with its
 * lumps aligned, and searching the entity lump. BSPX follows
 * fteqw-bulwark specs/bspx.txt; the header is looked for after the
 * standard header and after the last standard lump, and written after the
 * last standard lump, which is where map compilers put it.
 * ISC licence, see LICENSE. */
#include "bsptool.h"
#include "cmd.h"
#include "cvar.h"
#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	uint8_t *data;
	size_t len;
	int hdrsize;    /* bytes before the lump directory ends */
	int dirofs;     /* where the lump directory starts */
	int numlumps;
	struct {
		int ofs, len;
	} lumps[24];
	/* BSPX */
	int numx;
	struct {
		char name[24];
		int ofs, len;
	} x[64];
} bsp_t;

static int32_t rd32(const uint8_t *p) { return (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }
static void wr32(uint8_t *p, int32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)((uint32_t)v >> 24);
}

static bool read_bspx(bsp_t *b, size_t at)
{
	int n;
	if (at + 8 > b->len || memcmp(b->data + at, "BSPX", 4))
		return false;
	n = rd32(b->data + at + 4);
	if (n < 0 || n > (int)BW_ARRAY_COUNT(b->x) || at + 8 + (size_t)n * 32 > b->len)
		return false;
	for (int i = 0; i < n; i++) {
		const uint8_t *e = b->data + at + 8 + i * 32;
		memcpy(b->x[i].name, e, 23);
		b->x[i].name[23] = 0;
		b->x[i].ofs = rd32(e + 24);
		b->x[i].len = rd32(e + 28);
		if (b->x[i].ofs < 0 || b->x[i].len < 0 || (size_t)b->x[i].ofs + (size_t)b->x[i].len > b->len)
			return false;
	}
	b->numx = n;
	return true;
}

static bool parse_bsp(bsp_t *b, const char *name)
{
	int version;
	size_t end = 0;
	if (b->len < 8)
		return false;
	version = rd32(b->data);
	if (version == 29 || version == 30 || !memcmp(b->data, "BSP2", 4) || !memcmp(b->data, "2PSB", 4)) {
		b->dirofs = 4;
		b->numlumps = 15;
	} else if (!memcmp(b->data, "IBSP", 4) && rd32(b->data + 4) == 38) {
		b->dirofs = 8;
		b->numlumps = 19;
	} else if (!memcmp(b->data, "IBSP", 4) || !memcmp(b->data, "RBSP", 4) || !memcmp(b->data, "FBSP", 4)) {
		b->dirofs = 8;
		b->numlumps = !memcmp(b->data, "IBSP", 4) ? 17 : 18;
	} else {
		Con_Printf("%s isn't a BSP this engine knows\n", name);
		return false;
	}
	b->hdrsize = b->dirofs + b->numlumps * 8;
	if (b->len < (size_t)b->hdrsize)
		return false;
	for (int i = 0; i < b->numlumps; i++) {
		b->lumps[i].ofs = rd32(b->data + b->dirofs + i * 8);
		b->lumps[i].len = rd32(b->data + b->dirofs + i * 8 + 4);
		if (b->lumps[i].ofs < 0 || b->lumps[i].len < 0 || (size_t)b->lumps[i].ofs + (size_t)b->lumps[i].len > b->len) {
			Con_Printf("%s has a broken lump directory\n", name);
			return false;
		}
		end = BW_MAX(end, (size_t)b->lumps[i].ofs + (size_t)b->lumps[i].len);
	}
	{
		bool header_free = true;
		for (int i = 0; i < b->numlumps; i++)
			header_free &= b->lumps[i].ofs != b->hdrsize;
		if (!(header_free && read_bspx(b, (size_t)b->hdrsize)))
			read_bspx(b, (end + 3) & ~(size_t)3);
	}
	return true;
}

static bool load_bsp(const char *name, bsp_t *b)
{
	memset(b, 0, sizeof(*b));
	if (!(b->data = (uint8_t *)FS_LoadFile(name, &b->len))) {
		Con_Printf("couldn't open %s\n", name);
		return false;
	}
	if (!parse_bsp(b, name)) {
		Mem_Free(b->data);
		b->data = NULL;
		return false;
	}
	return true;
}

/* "maps/x.bsp" from "x", "maps/x" or a path */
static const char *bsp_path(const char *arg)
{
	static char path[256];
	if (!arg || !*arg) {
		const char *map = Cvar_String("mapname");
		if (!map[0])
			return NULL;
		snprintf(path, sizeof(path), "maps/%s.bsp", map);
		return path;
	}
	snprintf(path, sizeof(path), "%s%s%s", strchr(arg, '/') ? "" : "maps/", arg,
	         strlen(arg) > 4 && !Str_ICmp(arg + strlen(arg) - 4, ".bsp") ? "" : ".bsp");
	return path;
}

typedef struct {
	const char *name;
	const void *data;
	size_t len;
} xlump_t;

/* writes the standard lumps (4-aligned, in file order) and the BSPX lumps;
 * the old file is kept as .bak */
static bool write_bsp(const char *name, const bsp_t *b, const xlump_t *x, int numx)
{
	strbuf_t sb = {0};
	char path[1024], bak[1100];
	int order[24];
	FILE *f;
	static const uint8_t pad[32];
	bool backed_up = false;
	SB_Append(&sb, (const char *)(b->data), (size_t)b->hdrsize);
	for (int i = 0; i < b->numlumps; i++)
		order[i] = i;
	/* in the order they were in the file, so tools that assume it still work */
	for (int i = 1; i < b->numlumps; i++)
		for (int j = i; j > 0 && b->lumps[order[j]].ofs < b->lumps[order[j - 1]].ofs; j--) {
			int t = order[j];
			order[j] = order[j - 1];
			order[j - 1] = t;
		}
	for (int k = 0; k < b->numlumps; k++) {
		int i = order[k];
		SB_Append(&sb, (const char *)(pad), (4 - sb.len % 4) % 4);
		wr32((uint8_t *)sb.data + b->dirofs + i * 8, (int32_t)sb.len);
		wr32((uint8_t *)sb.data + b->dirofs + i * 8 + 4, b->lumps[i].len);
		SB_Append(&sb, (const char *)(b->data + b->lumps[i].ofs), (size_t)b->lumps[i].len);
	}
	SB_Append(&sb, (const char *)(pad), (4 - sb.len % 4) % 4);
	if (numx) {
		size_t hdr = sb.len, ofs;
		uint8_t e[32];
		SB_Append(&sb, "BSPX", 4);
		wr32(e, numx);
		SB_Append(&sb, (const char *)(e), 4);
		for (int i = 0; i < numx; i++)
			SB_Append(&sb, (const char *)(pad), 32);
		for (int i = 0; i < numx; i++) {
			SB_Append(&sb, (const char *)(pad), (4 - sb.len % 4) % 4);
			ofs = sb.len;
			SB_Append(&sb, x[i].data, x[i].len);
			memset(e, 0, sizeof(e));
			Str_Copy((char *)e, x[i].name, 24);
			wr32(e + 24, (int32_t)ofs);
			wr32(e + 28, (int32_t)x[i].len);
			memcpy(sb.data + hdr + 8 + i * 32, e, 32);
		}
		SB_Append(&sb, (const char *)(pad), (4 - sb.len % 4) % 4);
	}
	if (!FS_WritePath(name, path, sizeof(path)) || !FS_CreatePath(path)) {
		Con_Printf("can't write %s\n", name);
		SB_Free(&sb);
		return false;
	}
	snprintf(bak, sizeof(bak), "%s.bak", path);
	if ((f = fopen(path, "rb"))) {
		fclose(f);
		remove(bak);
		backed_up = !rename(path, bak);
	}
	if (!(f = fopen(path, "wb"))) {
		Con_Printf("can't write %s\n", name);
		SB_Free(&sb);
		return false;
	}
	fwrite(sb.data, 1, sb.len, f);
	fclose(f);
	Con_Printf("wrote %s (%zu bytes)%s\n", name, sb.len, backed_up ? va(", the old file is %s.bak", name) : "");
	SB_Free(&sb);
	return true;
}

/* the file's BSPX lumps, with some replaced, added or taken away */
static int collect_x(const bsp_t *b, xlump_t *out, int max, const char *drop, const xlump_t *add, int numadd)
{
	int n = 0;
	for (int i = 0; i < b->numx && n < max; i++) {
		bool replaced = drop && !Str_ICmp(b->x[i].name, drop);
		for (int k = 0; k < numadd; k++)
			replaced |= !Str_ICmp(b->x[i].name, add[k].name);
		if (replaced)
			continue;
		out[n].name = b->x[i].name;
		out[n].data = b->data + b->x[i].ofs;
		out[n].len = (size_t)b->x[i].len;
		n++;
	}
	for (int k = 0; k < numadd && n < max; k++)
		out[n++] = add[k];
	return n;
}

bool BSPX_SetLumps(const char *bspname, const char *const *names, const void *const *datas, const size_t *lens, int count)
{
	bsp_t b;
	xlump_t add[16], all[80];
	int n;
	bool ok;
	if (!load_bsp(bspname, &b))
		return false;
	for (int i = 0; i < count && i < 16; i++)
		add[i] = (xlump_t){names[i], datas[i], lens[i]};
	n = collect_x(&b, all, 80, NULL, add, BW_MIN(count, 16));
	ok = write_bsp(bspname, &b, all, n);
	Mem_Free(b.data);
	return ok;
}

/* --- commands ------------------------------------------------------------------ */

static void BspxList_f(int argc, const char **argv)
{
	const char *name = bsp_path(argc > 1 ? argv[1] : NULL);
	bsp_t b;
	if (!name) {
		Con_Printf("mod_bspx_list <bsp>: the extension lumps in a map\n");
		return;
	}
	if (!load_bsp(name, &b))
		return;
	if (!b.numx)
		Con_Printf("%s has no BSPX lumps\n", name);
	for (int i = 0; i < b.numx; i++)
		Con_Printf("%-24s %8d bytes\n", b.x[i].name, b.x[i].len);
	Mem_Free(b.data);
}

static void BspxStrip_f(int argc, const char **argv)
{
	bsp_t b;
	xlump_t all[80];
	const char *name;
	int n;
	if (argc < 3) {
		Con_Printf("mod_bspx_strip FILENAME NAME: removes an extended lump from the named bsp file\n");
		return;
	}
	name = bsp_path(argv[1]);
	if (!load_bsp(name, &b))
		return;
	n = collect_x(&b, all, 80, argv[2], NULL, 0);
	if (n == b.numx)
		Con_Printf("%s has no %s lump\n", name, argv[2]);
	else
		write_bsp(name, &b, all, n);
	Mem_Free(b.data);
}

/* .lit and .lux files: an 8 byte header ("QLIT" and a version), then the samples */
static void *load_side(const char *bsp, const char *ext, size_t *len)
{
	char path[256];
	uint8_t *d;
	snprintf(path, sizeof(path), "%.*s.%s", (int)(strlen(bsp) - 4), bsp, ext);
	if (!(d = (uint8_t *)FS_LoadFile(path, len)))
		return NULL;
	if (*len < 8 || memcmp(d, "QLIT", 4)) {
		Con_Printf("%s doesn't start with QLIT; left out\n", path);
		Mem_Free(d);
		return NULL;
	}
	Con_Printf("adding %s\n", path);
	*len -= 8;
	memmove(d, d + 8, *len);
	return d;
}

static void BspxInjest_f(int argc, const char **argv)
{
	bsp_t b;
	xlump_t add[3], all[80];
	void *data[3] = {0};
	int numadd = 0;
	const char *name;
	if (argc < 2) {
		Con_Printf("mod_bspx_injest <bsp>: puts the map's .lit and .lux files in it as BSPX lumps (keep a backup)\n");
		return;
	}
	name = bsp_path(argv[1]);
	if (!load_bsp(name, &b))
		return;
	{
		static const char *const exts[][2] = {{"lit", "RGBLIGHTING"}, {"lux", "LIGHTINGDIR"}, {"dlit", "LIGHTINGDIR"}};
		for (size_t i = 0; i < BW_ARRAY_COUNT(exts); i++) {
			size_t len;
			bool have = false;
			for (int k = 0; k < numadd; k++)
				have |= !strcmp(add[k].name, exts[i][1]);
			if (have || !(data[numadd] = load_side(name, exts[i][0], &len)))
				continue;
			add[numadd] = (xlump_t){exts[i][1], data[numadd], len};
			numadd++;
		}
	}
	if (!numadd)
		Con_Printf("no .lit or .lux files beside %s\n", name);
	else
		write_bsp(name, &b, all, collect_x(&b, all, 80, NULL, add, numadd));
	for (int i = 0; i < numadd; i++)
		Mem_Free(data[i]);
	Mem_Free(b.data);
}

static void Realign_f(int argc, const char **argv)
{
	bsp_t b;
	xlump_t all[80];
	const char *name = bsp_path(argc > 1 ? argv[1] : NULL);
	if (!name) {
		Con_Printf("mod_realign <bsp>: rewrite a map with its lumps aligned to 4 bytes\n");
		return;
	}
	if (!load_bsp(name, &b))
		return;
	write_bsp(name, &b, all, collect_x(&b, all, 80, NULL, NULL, 0));
	Mem_Free(b.data);
}

/* mod_showent [number or text]: the map's entities, one or those matching */
static void Showent_f(int argc, const char **argv)
{
	bsp_t b;
	const char *name = bsp_path(NULL);
	int num = 0, shown = 0;
	bool bynum = argc > 1 && argv[1][0] >= '0' && argv[1][0] <= '9';
	char *text;
	if (!name) {
		Con_Printf("no map running\n");
		return;
	}
	if (!load_bsp(name, &b))
		return;
	/* the entities are the first lump in every family */
	text = Mem_Alloc((size_t)b.lumps[0].len + 1);
	memcpy(text, b.data + b.lumps[0].ofs, (size_t)b.lumps[0].len);
	for (char *p = text; (p = strchr(p, '{')); num++) {
		char *end = strchr(p, '}');
		size_t len;
		if (!end)
			break;
		len = (size_t)(end - p);
		if (argc < 2 ? num == 0 : bynum ? num == atoi(argv[1]) : false) {
			Con_Printf("{\t//%d%.*s}\n", num, (int)len - 1, p + 1);
			shown++;
		} else if (argc > 1 && !bynum) {
			char save = end[0];
			end[0] = 0;
			if (strstr(p, argv[1])) {
				Con_Printf("{\t//%d%s}\n", num, p + 1);
				shown++;
			}
			end[0] = save;
		}
		p = end + 1;
	}
	if (!shown)
		Con_Printf("no entity %s of %d\n", argc > 1 ? argv[1] : "0", num);
	Mem_Free(text);
	Mem_Free(b.data);
}

void BSP_InitTools(void)
{
	Cmd_AddD("mod_bspx_list", BspxList_f, "mod_bspx_list [bsp]: a map's BSPX lumps and their sizes");
	Cmd_AddD("mod_bspx_strip", BspxStrip_f, "mod_bspx_strip <bsp> <lump>: take a BSPX lump out of a map");
	Cmd_AddD("mod_bspx_injest", BspxInjest_f, "mod_bspx_injest <bsp>: put a map's .lit/.lux files into it as BSPX lumps");
	Cmd_AddD("mod_realign", Realign_f, "mod_realign [bsp]: rewrite a map with only its lump alignment changed");
	Cmd_AddD("mod_showent", Showent_f, "mod_showent [number or text]: the running map's entities, by number or containing text");
}
