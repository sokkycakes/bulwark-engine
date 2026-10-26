/* Bulwark engine - renderer internals. ISC licence, see LICENSE.
 *
 * The layout follows the split Source uses, written fresh: a material
 * system (images, materials, GLSL programs), a world renderer for BSP
 * surfaces and lightmap pages, a model renderer, a 2D layer for the client
 * game's HUD and menus, and a view/scene front end driven by csprogs. */
#ifndef BW_R_LOCAL_H
#define BW_R_LOCAL_H

#include "render.h"
#include "r_gl.h"
#include "../client/client.h"
#include "../common/cvar.h"

/* --- images -------------------------------------------------------------- */

typedef struct image_s {
	char name[128];
	GLuint tex;
	GLenum target;
	int width, height;
	bool alpha;   /* has transparent pixels */
	bool missing; /* no file was found: the checker texture stands in */
	struct image_s *next;
} image_t;

#define IMG_CLAMP 1
#define IMG_NOMIP 2
#define IMG_NEAREST 4

image_t *R_FindImage(const char *name, int flags); /* never NULL */
image_t *R_ImageFromPixels(const char *name, const uint8_t *rgba, int w, int h, int flags);
image_t *R_LoadCubemap(const char *basename);    /* basename_rt.tga ... or NULL */
bool R_ImageExists(const char *name);
const uint8_t *R_Palette(void);
void R_InitImages(void);
void R_ShutdownImages(void);
extern image_t *r_whiteimage, *r_blackimage, *r_checkerimage, *r_flatnormal, *r_whitecube;
/* decodes an image file into RGBA (Mem_Free the result) */
uint8_t *R_LoadPixels(const char *name, int *w, int *h);
uint8_t *R_DecodeImage(const uint8_t *data, size_t len, int *w, int *h);

/* --- materials ----------------------------------------------------------- */

enum { MP_DEFAULT, MP_WALL, MP_SKIN, MP_UNLIT, MP_SKY, MP_WATER };

enum { BLEND_OPAQUE, BLEND_ALPHA, BLEND_ADD, BLEND_FILTER, BLEND_CUSTOM };

#define MAT_MAX_STAGES 6
typedef struct {
	image_t *image;
	bool lightmap;   /* $lightmap */
	int blend;
	GLenum src, dst;
	float color[4];  /* rgbGen/alphaGen const */
	bool vertexcolor;
	float scroll[2], scale[2], rotate;
	bool alphatest;
} mstage_t;

typedef struct material_s {
	char name[128];
	int program;
	image_t *diffuse, *normal, *fullbright, *specular;
	mstage_t stages[MAT_MAX_STAGES];
	int numstages;
	int blend;            /* for the whole surface when drawn with a program */
	GLenum src, dst;
	bool alphatest;
	bool twosided;
	bool nodraw;          /* caulk, clip, triggers... */
	bool sky;
	bool polygonoffset;
	bool fromfile;        /* came from a .mat or shader script */
	image_t *skybox;      /* cubemap for sky materials */
	float alpha;
	int width, height;    /* the size drawgetimagesize reports */
	struct material_s *next;
} material_t;

#define MATF_2D 1    /* defaults suit a HUD picture: no lightmap */
#define MATF_MODEL 2 /* defaults suit a model skin */
material_t *R_FindMaterial(const char *name, int flags);
material_t *R_MaterialFromText(const char *name, const char *text, int flags); /* unless it exists */
bool R_MaterialFileExists(const char *name);
/* numeric handles for shaderforname and .forceshader; 0 is none */
int R_MaterialHandle(material_t *m);
material_t *R_MaterialForHandle(int h);
material_t *R_MaterialForImage(const char *name, image_t *img, int flags);
void R_InitMaterials(void);
void R_ShutdownMaterials(void);

/* --- GLSL programs ------------------------------------------------------- */

enum { ATTR_POS, ATTR_TC, ATTR_LMTC, ATTR_NORMAL, ATTR_COLOR, ATTR_COUNT };

typedef struct {
	GLuint prog;
	GLint u_mvp, u_model, u_tex, u_lightmap, u_cube, u_color, u_lmscale, u_vieworg, u_light_ambient;
	GLint u_light_dir, u_light_diffuse, u_tcmod, u_alphatest, u_fog, u_time, u_dlights, u_numdlights;
	GLint u_dlightcolors, u_fullbright, u_usefullbright;
} glprog_t;

enum { PROG_2D, PROG_WORLD, PROG_MODEL, PROG_UNLIT, PROG_SKY, PROG_COUNT };
extern glprog_t r_progs[PROG_COUNT];
bool R_InitPrograms(void);
void R_ShutdownPrograms(void);
void R_UseProgram(int p);

/* --- geometry ------------------------------------------------------------ */

/* the one vertex layout used for 3D geometry */
typedef struct {
	float xyz[3];
	float st[2];
	float lm[2];
	float normal[3];
	uint8_t color[4];
} rvert_t;

void R_SetupVertexAttribs(void); /* for rvert_t in the bound buffer */
void R_ShutdownStream(void);

/* streamed geometry (polygons, sprites, 2D) */
void R_StreamDraw(GLenum mode, const rvert_t *verts, int numverts, const uint16_t *idx, int numidx);

/* GL state, cached */
void GL_SetBlend(int blend, GLenum src, GLenum dst);
void GL_SetDepth(bool test, bool write);
void GL_SetCull(bool cull);
void GL_BindTexture(int unit, GLenum target, GLuint tex);

/* --- models -------------------------------------------------------------- */

typedef enum { MOD_NONE, MOD_BRUSH, MOD_ALIAS, MOD_IQM, MOD_SPRITE } modtype_t;

typedef struct {
	material_t *material;
	int lightmap;     /* page, -1 for none */
	int firstindex, numindexes;
	float mins[3], maxs[3];
	int type;         /* BSP surface type */
	int orig;         /* index in the file, before sorting */
	int visframe;     /* marked visible in this frame */
} rsurface_t;

struct alias_s;
struct iqm_s;
struct sprite_s;

typedef struct rmodel_s {
	char name[128];
	modtype_t type;
	float mins[3], maxs[3];
	float radius;
	/* brush models: a range of the world's surfaces */
	int firstsurface, numsurfaces;
	struct alias_s *alias;
	struct iqm_s *iqm;
	struct sprite_s *sprite;
	int numframes;
	bool loaded, failed;
} rmodel_t;

rmodel_t *R_ModelForIndex(int index);
rmodel_t *R_LoadModel(const char *name); /* cached by name */
void R_FreeModels(void);
int R_ModelNumFrames(rmodel_t *m);

/* --- world --------------------------------------------------------------- */

typedef struct {
	bool loaded;
	char name[128];
	rsurface_t *surfaces;
	int numsurfaces;
	GLuint vao, vbo, ebo;
	GLuint *lightmaps;
	int numlightmaps;
	int lightmapsize;
	bool deluxe;       /* odd pages hold light directions */
	rmodel_t *submodels;
	int numsubmodels;
	/* light grid for lighting models (Q3 BSP) */
	uint8_t *lightgrid;
	int gridbounds[3];
	float gridorigin[3], gridsize[3];
	char skyname[64];
	image_t *skybox;
	float fog[4];
	bool hasfog;
	/* density r g b alpha depthbias, from worldspawn or waterfog/skyroomfog */
	float waterfog[6], skyroomfog[6];
	/* visibility: the collision map's tree and leafs, and each file surface's
	 * place in the sorted list */
	bspmap_t *bsp;
	int *surfmap;
	int visframe;
	int viewcluster;
} rworld_t;

extern rworld_t r_world;
void R_LoadWorld(bspmap_t *bsp, const char *path);
void R_FreeWorld(void);
void R_WorldUpload(const rvert_t *v, int numverts, const uint32_t *idx, int numidx);
bool R_LoadQ1World(bspmap_t *bsp, const char *path, const uint8_t *d, size_t size);
void R_FreeQ1World(void);
bool R_Q1LightPoint(const float *p, float *ambient, float *diffuse, float *dir);

/* --- decals and surface queries (r_decal.c) -------------------------------- */

void R_WorldKeepGeometry(const rvert_t *v, int numverts, const uint32_t *idx, int numidx);
void R_FreeWorldGeometry(void);
int R_SurfacesInBox(const float *mins, const float *maxs, int *out, int max);
int R_SurfaceNearPoint(const rmodel_t *m, const float *p);
const rsurface_t *R_ModelSurface(const rmodel_t *m, int s);
int R_SurfacePoints(const rsurface_t *rs, const rvert_t **out, int max);
const rvert_t *R_SurfaceTriangle(const rsurface_t *rs, int tri, int corner);
void R_SurfaceAxes(const rsurface_t *rs, float *saxis, float *taxis, float *normal);
void R_SpawnDecal(material_t *mat, int blend, GLenum src, GLenum dst, const float *org, const float *normal,
                  float radius, float angle, const float *tc, const float *rgba, float alphadelta, float life);
void R_AddDecal(material_t *mat, const float *org, const float *up, const float *side, const float *rgb, float alpha);
void R_ClearTempDecals(void);
void R_ClearDecals(void);
void R_UpdateDecals(void);
void R_DrawDecals(void);
void R_InitDecals(void);
void R_PickBlend(const material_t *m, int blend, GLenum src, GLenum dst, int *ob, GLenum *os, GLenum *od);
void R_BlendColor(GLenum src, const float *rgba, float *out);

/* --- particles (r_part.c) ------------------------------------------------ */

const char *R_ParticleEffectName(int index);
void R_UpdateParticles(void);
void R_AddParticleLights(void);
void R_DrawParticles(void);
void R_ClearParticles(void);
void R_InitParticles(void);
void R_ShutdownParticles(void);
void R_DrawWorldModel(rmodel_t *m, const float *mvp, const float *model, float alpha, bool translucentpass);
void R_DrawSky(const float *viewproj_noorigin);
void R_LightPoint(const float *p, float *ambient, float *diffuse, float *dir);

/* --- scene --------------------------------------------------------------- */

typedef struct {
	int entnum;
	rmodel_t *model;
	int modelindex;
	float origin[3];
	float angles[3];
	float axis[3][3];
	float scale;
	float alpha;
	float colormod[3];
	float glowmod[3];
	int frame, frame2;
	float lerpfrac;
	float frame1time, frame2time;
	int skin;
	int renderflags;
	int effects;
	int drawflags;
	material_t *forceshader;
	struct qcvm_s *vm;
	int qcent;        /* entity in that vm */
	int skeletonindex; /* a skeleton object of that vm posing the model, 0 for none */
} rentity_t;

typedef struct {
	float origin[3];
	float radius;
	float color[3];
} rdlight_t;

#define R_MAX_ENTITIES 4096
#define R_MAX_DLIGHTS 64

typedef struct {
	float vx, vy, vw, vh;        /* viewport, in virtual pixels */
	float fov_x, fov_y, afov;
	float origin[3], angles[3];
	float nearclip, farclip;
	bool drawworld;
	bool perspective;
	int viewentity;
	float cl_viewangles[3];
	rentity_t ents[R_MAX_ENTITIES];
	int numents;
	rdlight_t dlights[R_MAX_DLIGHTS];
	int numdlights;
	/* matrices for the frame being drawn */
	float view[16], proj[16], viewproj[16];
	float vmviewproj[16]; /* the projection view models use */
	float forward[3], right[3], up[3];
	/* the last view that filled most of the screen, for console tests and
	 * the screenshots that redraw it */
	float mainorigin[3], mainforward[3], mainangles[3], mainfov[2];
	float mainviewproj[16], mainvp[4]; /* its matrix and viewport (virtual pixels) */
	bool havemain, maindrawworld;
	/* VF_SKYROOM_CAMERA: where the skyroom is seen from, through the sky */
	bool skyroom;
	float skyroom_origin[3];
	bool noskyroom; /* set while drawing the skyroom itself */
} rscene_t;

/* the fog for the view being drawn: rgb and density */
extern float r_viewfog[4];

extern rscene_t r_scene;

/* counters for r_speeds, per view */
typedef struct {
	int worldsurfs, culled, entities, polys, particles;
} rstats_t;
extern rstats_t r_stats;
extern double r_time;

void R_ClearScene(void);
rentity_t *R_AllocEntity(void);
void R_RenderScene(void);
void R_DrawModelEntity(rentity_t *e, bool translucentpass);
void R_DrawPolygons(bool twod);
void R_BeginPolygon(const char *shader, int flags, bool twod);
void R_PolygonVertex(const float *org, const float *tc, const float *rgb, float alpha);
void R_EndPolygon(void);
void R_AddEntityFromQC(struct qcvm_s *vm, int ent);
void R_AddEntities(struct qcvm_s *vm, int mask);
int R_AddDynamicLight(const float *org, float radius, const float *color);
void R_SetupView(void);
void R_Project(const float *in, float *out);
void R_Unproject(const float *in, float *out);
void R_InitScene(void);
int R_FrameForName(rmodel_t *m, const char *name);
const char *R_FrameToName(rmodel_t *m, int frame);
float R_FrameDuration(rmodel_t *m, int frame);

/* --- 2D ------------------------------------------------------------------ */

void R_Begin2D(int w, int h);
void R_End2D(void);
void R_Flush2D(void);
void R_DrawPic(float x, float y, float w, float h, material_t *mat, float s1, float t1, float s2, float t2,
               const float *rgb, float alpha, int flags);
void R_DrawOverlays(int w, int h);
void R_DrawFill(float x, float y, float w, float h, const float *rgb, float alpha, int flags);
float R_DrawString(float x, float y, const char *text, float sx, float sy, const float *rgb, float alpha,
                   int flags, bool markup);
float R_StringWidth(const char *text, float sx, float sy, bool markup);
void R_SetClip(float x, float y, float w, float h);
void R_ResetClip(void);
void R_DrawConsole(int w, int h);
void R_InitDraw(void);
int R_LoadFont(const char *name, const char *path, const char *sizes, int slot);
int R_FindFont(const char *name);
void R_SetDrawFont(int slot);
void R_ShutdownDraw(void);
extern int r_vwidth, r_vheight; /* virtual 2D screen size */
extern int r_pwidth, r_pheight; /* physical framebuffer size */
extern float r_2dscale;

/* --- maths --------------------------------------------------------------- */

void Mat4_Identity(float *m);
void Mat4_Multiply(float *out, const float *a, const float *b);
void Mat4_Perspective(float *m, float fovx, float fovy, float znear, float zfar);
void Mat4_Ortho(float *m, float l, float r, float b, float t, float n, float f);
void Mat4_FromAxis(float *m, const float axis[3][3], const float *origin, float scale);
void Mat4_View(float *m, const float *origin, const float *angles);
void Mat4_Transform(const float *m, const float *in, float *out4);
bool Mat4_Invert(float *out, const float *m);
void AnglesToAxis(const float *angles, float axis[3][3]);

/* --- tools (r_tools.c) and the accessors they use ------------------------ */

void R_ForEachImage(void (*fn)(image_t *img, void *ctx), void *ctx);
void R_ForEachMaterial(void (*fn)(material_t *m, void *ctx), void *ctx);
int R_NumModels(void);
rmodel_t *R_ModelAt(int i);
size_t R_ModelMemory(const rmodel_t *m, int *verts, int *tris);
void R_ForEachWadTexture(void (*fn)(const char *wad, const char *name, int type, int size, void *ctx), void *ctx);
int R_NumWads(void);
bool R_ProgramSource(int p, const char **name, const char **vs, const char **fs);
const char *R_GLSLHeader(void);
bool R_FontInfo(int slot, const char **name, bool *truetype, int *sizes, int *numsizes);
bool R_ToolsRedrawing(void);   /* a screenshot tool is drawing the scene again */
void R_DrawToolsWorld(void);   /* pointfile and route lines, in the 3D pass */
void R_DrawToolsOverlay(int w, int h); /* showpic pictures */
void R_ToolsEndFrame(void);    /* the cubemap, 360, stereo and mega screenshots */
void R_InitTools(void);

/* r_rtlights.c */
void R_InitRTLights(void);
void R_LoadRTLights(void);
void R_ClearRTLights(void);
void R_AddRTLights(void);
void R_DrawRTLightsOverlay(int w, int h);
void R_DrawHud(int w, int h); /* r_hud.c */
void R_ReloadImage(const char *name);
void R_ToolsMapLoaded(void);  /* skygroup overrides */

extern cvar_t *r_lightmapscale, *r_fullbright, *r_drawentities, *r_drawworld, *r_novis, *r_clearcolor;
extern cvar_t *gl_texturemode, *r_wireframe, *r_speeds;

#endif
