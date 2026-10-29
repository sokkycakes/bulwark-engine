/* Bulwark voxel world builtins (engine: src/server/sv_voxel.c,
 * docs/spec/voxel.md). Bound by name, so the numbers are placeholders.
 * Cells are addressed by integer vectors '[x, y, z]'. Server progs get every
 * call; client progs get the read-only ones (marked "both"). */

/* shapes (vxshape_t) */
#define VXS_EMPTY	0
#define VXS_BOX		1
#define VXS_SLOPE	2	/* rises towards +X at rotation 0 */
#define VXS_RSLOPE	3
#define VXS_SLAB_LOW	4
#define VXS_SLAB_HIGH	5
#define VXS_THIN	6	/* panel on the +X side at rotation 0 */
#define VXS_FIELD	7	/* full cell, contents only (water, ladders, volumes) */

/* light kinds (CSO VoxelLightType) */
#define VXL_NONE	0
#define VXL_BLOCKED	1
#define VXL_SLOPE_BLOCKED	2
#define VXL_POINT	3
#define VXL_SKY	4
#define VXL_WATER_BLOCKED	5
#define VXL_SLAB_BLOCKED	6

/* block flags */
#define VXF_NODRAW	1
#define VXF_FULLBRIGHT	2

/* content bits (Q3 style) */
#define VXC_SOLID	1
#define VXC_WATER	32
#define VXC_LADDER	16384
#define VXC_PLAYERCLIP	65536

#if defined(SERVER) || defined(QWSSQC)
float voxel_create(vector cells, float blocksize, vector origin) = #0:voxel_create;
void voxel_free(void) = #0:voxel_free;
float voxel_define(float type, string name, float shape, float contents, vector rgb, float alpha, string texture, float flags) = #0:voxel_define;
/* how a defined type passes light (VXL_*) and the light it gives, 0..15 per
   channel; call after voxel_define, which resets both. Newer engines only:
   guard with checkbuiltin(voxel_setlight). */
float voxel_setlight(float type, float lighttype, vector rgb) = #0:voxel_setlight;
float voxel_set(vector cell, float type, float rot) = #0:voxel_set;
float voxel_fill(vector mincell, vector maxcell, float type, float rot) = #0:voxel_fill;
float voxel_save(string name, string meta) = #0:voxel_save;	/* data/<name>, keeps <name>.bak */
float voxel_load(string name) = #0:voxel_load;			/* replaces the world on success */
string voxel_meta(void) = #0:voxel_meta;			/* metadata of the last load */
string voxel_error(void) = #0:voxel_error;			/* why the last save/load failed */
float voxel_unknown(void) = #0:voxel_unknown;			/* cells of unknown types in the last load */
#endif

/* both */
float voxel_get(vector cell) = #0:voxel_get;
float voxel_getrot(vector cell) = #0:voxel_getrot;
vector voxel_cellforpoint(vector point) = #0:voxel_cellforpoint;
vector voxel_cellorigin(vector cell) = #0:voxel_cellorigin;	/* minimum corner */
vector voxel_size(void) = #0:voxel_size;			/* cells; '0 0 0' when there is no world */
float voxel_blocksize(void) = #0:voxel_blocksize;
float voxel_count(void) = #0:voxel_count;
float voxel_typeforname(string name) = #0:voxel_typeforname;	/* -1 when unknown */
string voxel_typename(float type) = #0:voxel_typename;
vector voxel_tracecell(void) = #0:voxel_tracecell;		/* cell the last trace hit, '-1 -1 -1' if none */
float voxel_tracetype(void) = #0:voxel_tracetype;		/* its type, -1 if none */
