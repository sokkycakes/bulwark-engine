/* Bulwark engine - native UI: what its parts share. ISC licence, see LICENSE.
 *
 * ui_native.c is the ui.h backend for builds without PanoramaEngine (the
 * Xbox): it reports Stiletto's five WebCore layouts as present, so
 * cl_webcore.c routes its title, lobby, pause, hero picker and HUD exactly
 * as it does on the PC, and draws each of them in C. One file per screen:
 *
 *   uin_title.c   title, Create and Join dialogs, quit confirmation
 *   uin_lobby.c   lobby, Leave confirmation, read-only chat
 *   uin_pause.c   in-game pause page
 *   uin_hero.c    hero picker (ql-menu arena port and the other modes)
 *   uin_hud.c     the HUD (UI_EMBED view drawn by UI_DrawAt)
 *   uin_misc.c    loading screen, error dialog, button-hint footer
 *   uin_draw.c    the 2D batch and its primitives (d_*)
 *   uin_atlas.c   the glyph and image atlas: baker, cache, lookups
 *
 * Link rule: bwui sits below bwclient, so these files call only common/
 * (Cvar, Cmd, Cbuf, FS, JSON, Mem, Sys), render.h and the query handler
 * cl_webcore.c gives UI_SetQueryHandler. All client state comes through
 * uin_query (the same requests the pages made with fte_query) or cvars.
 *
 * Memory: no steady heap. Screen state lives in its view's fixed block
 * (UIN_STATE), JSON trees live for one call, the batch is static. */
#ifndef BW_UIN_LOCAL_H
#define BW_UIN_LOCAL_H

#include "ui.h"
#include "uin_stiletto.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../render/render.h"

/* --- ids from the manifest (uin_stiletto.h) ------------------------------------- */

#define UIN_X_ID(id, ...) id,
enum { UIN_FACES(UIN_X_ID) UIN_NUM_FACES };
enum { UIN_IMAGES(UIN_X_ID) UIN_NUM_REGIONS };
#undef UIN_X_ID
#define UIN_NUM_PAGES 2

/* --- views and screens ------------------------------------------------------------- */

/* what a layout is drawn as (ui_native.c's layout table) */
enum {
	UIN_KIND_NONE,
	UIN_KIND_TITLE, /* layout/web/title-menu-v2.xml, title-menu.xml */
	UIN_KIND_LOBBY, /* layout/web/lobby-menu.xml */
	UIN_KIND_PAUSE, /* layout/web/pause-menu.xml */
	UIN_KIND_HERO,  /* layout/web/ql-menu.xml (search "?mode=arena") */
	UIN_KIND_HUD,   /* layout/web/hud.xml, a UI_EMBED view */
	UIN_NUM_KINDS
};

/* what a key means to a menu. Screens get the action and the raw key, so
 * one that must tell Esc from B (the pause page) can. */
enum {
	UIN_A_NONE,   /* any other key: still swallowed while a menu is open */
	UIN_A_UP,     /* d-pad, left stick (stick_nav), arrows, W, mouse wheel */
	UIN_A_DOWN,   /* d-pad, left stick, arrows, S, mouse wheel */
	UIN_A_LEFT,   /* d-pad, left stick, arrows */
	UIN_A_RIGHT,  /* d-pad, left stick, arrows */
	UIN_A_ACCEPT, /* pad A, Enter, Space */
	UIN_A_BACK,   /* pad B, Esc, Backspace */
	UIN_A_X,      /* pad X */
	UIN_A_Y,      /* pad Y */
	UIN_A_START   /* pad START (K_GP_MENU) */
};

/* bytes of per-view screen state; screens cast it with UIN_STATE */
#define UIN_STATE_BYTES 4096
/* the fade-in after a view opens; keys are ignored until it is over
 * (title-menu-v2.css .title-chrome 0.18 s) */
#define UIN_FADE_S 0.18

typedef struct uin_view_s uin_view_t;

/* A screen: one per kind, defined in its uin_*.c file. Every callback but
 * draw may be NULL. They run on the main thread, inside ui.h calls.
 *
 * Re-entrancy: a query can close the view that made it (arena_action:close
 * and spectate call close_menu, which calls UI_Close). uin_query returns
 * NULL then: return at once and touch nothing of v (its state may be
 * reused). Commands queued with uin_cmd run later and never re-enter. */
typedef struct {
	const char *name;
	/* v->state is zeroed and v->search filled before this */
	void (*open)(uin_view_t *v);
	void (*close)(uin_view_t *v);
	/* a key press, or an auto-repeat of a held direction (repeat true), once
	 * the fade-in is over; releases aren't passed on */
	void (*key)(uin_view_t *v, int action, int key, bool repeat);
	/* every frame while open (UI_Frame), before any drawing */
	void (*frame)(uin_view_t *v, double dt);
	/* menus get the whole screen in virtual pixels; the HUD gets the
	 * rectangle UI_DrawAt was given, and its tint in v->rgb, v->alpha */
	void (*draw)(uin_view_t *v, float x, float y, float w, float h);
} uin_screen_t;

struct uin_view_s {
	bool open;
	int kind;          /* UIN_KIND_* */
	int flags;         /* UI_INPUT, UI_EMBED */
	int serial;        /* new on every open: how uin_query tells the view died */
	const uin_screen_t *screen;
	double opened;     /* uin_now() when it opened */
	float fade;        /* 0 to 1 over UIN_FADE_S after opening */
	float rgb[3];      /* the HUD's tint from UI_DrawAt (1 1 1 otherwise) */
	float alpha;
	char layout[48];
	char search[64];   /* the page's location.search, e.g. "?mode=arena" */
	union {
		double align;
		uint8_t bytes[UIN_STATE_BYTES];
	} state;
};

/* the view's state as a screen's own struct, checked to fit at compile time */
#define UIN_STATE(v, type) ((type *)(v)->state.bytes + 0 * sizeof(char[sizeof(type) <= UIN_STATE_BYTES ? 1 : -1]))

extern const uin_screen_t uin_title_screen; /* uin_title.c */
extern const uin_screen_t uin_lobby_screen; /* uin_lobby.c */
extern const uin_screen_t uin_pause_screen; /* uin_pause.c */
extern const uin_screen_t uin_hero_screen;  /* uin_hero.c */
extern const uin_screen_t uin_hud_screen;   /* uin_hud.c */

/* --- ui_native.c: services for screens --------------------------------------------- */

extern cvar_t *ui_native_safe;     /* TV-safe inset, 0.05 on the Xbox, else 0 */
extern cvar_t *ui_hud_style;       /* 0 the PC port's HUD, 1 Figma extras */
extern cvar_t *ui_hud_debug;       /* 1 outlines the HUD's cluster boxes */
extern cvar_t *ui_native_hudtest;  /* 1 forces the HUD fixture values */
extern cvar_t *ui_native_stats;    /* 1 prints draw timings every 2 s */

/* seconds, advanced by UI_Frame (scripted runs step it like the host clock) */
double uin_now(void);
/* a view of this UIN_KIND_* is open */
bool uin_kind_open(int kind);

/* fte_query(request) as the pages made it: the reply, valid until the next
 * uin_query. NULL when the call closed v (see uin_screen_t); v may be NULL
 * for requests that belong to no view (the HUD's). Never NULL otherwise:
 * "" when nothing answers. */
const char *uin_query(uin_view_t *v, const char *request);
/* console text, queued (Cbuf_AddText): the native UI is trusted C, so the
 * page allow list doesn't apply. End each command with \n. */
void uin_cmd(const char *text);

enum { UIN_SND_MOVE = 1, UIN_SND_ACCEPT = 2, UIN_SND_BACK = 3 };
void uin_sound(int which); /* localsound:misc/menu1|2|3 */

/* a key=value of v->search ("?mode=arena&x=1"); false if it isn't there */
bool uin_search_get(const uin_view_t *v, const char *key, char *out, size_t outsize);

/* the screen inset by ui_native_safe on each side, in virtual pixels */
void uin_safe_rect(float *x, float *y, float *w, float *h);

/* the menus' focus-bar easing, cubic-bezier(.2,.8,.2,1)
 * (title-menu-v2.css:83-85), t and the result 0 to 1 */
float uin_ease_focus(float t);

/* a labelled outline box (stub screens, debugging) */
void uin_debug_box(float x, float y, float w, float h, const char *label, uint32_t rgba);

/* --- uin_misc.c: shared widgets ----------------------------------------------------- */

/* the loading screen while cl_loadstage is set; true if it drew (P3: needs
 * the loading_hook call in cl_menu.c to show during a blocking load) */
bool uin_loading_draw(float w, float h);

/* the button-hint footer, right-aligned at UIN_FOOTER_RIGHT on the baseline
 * UIN_FOOTER_BASELINE (scaled to the screen): spec lists button=label
 * pairs, "A=SELECT B=BACK X=READY"; labels use '_' for spaces */
void uin_footer(float w, float h, const char *spec);
/* one button prompt (disc and label) ending at right on the baseline base,
 * through the current transform, for a dialog's own hints; returns its
 * left edge */
float uin_prompt(float right, float base, const char *button, const char *label, float alpha);

/* the modal error dialog over every view: opened by a screen (title after
 * takedisconnect, lobby on an error), it takes keys first until closed */
void uin_error_open(const char *title, const char *text);
bool uin_error_active(void);
void uin_error_key(int action, int key);
void uin_error_draw(float w, float h);

/* --- uin_draw.c: the 2D batch -------------------------------------------------------
 * Primitives append quads to one static batch (2048 vertices) and draw it
 * with R_Draw2DTriangles when the page or blend changes, when it is full,
 * on a clip change and at d_flush. Draw a frame's pieces page by page
 * where order allows: each change of page is one more draw call.
 * Positions are virtual pixels through the current transform (d_xf). */

/* colours are 0xRRGGBBAA */
#define UIN_RGBA(r, g, b, a) (((uint32_t)(r) << 24) | ((uint32_t)(g) << 16) | ((uint32_t)(b) << 8) | (uint32_t)(a))
/* 0xRRGGBB with alpha 0-1, as CSS rgba() writes it */
#define UIN_HEXA(rgb, a) (((uint32_t)(rgb) << 8) | (uint32_t)((a) * 255.0f + 0.5f))
uint32_t uin_col_alpha(uint32_t rgba, float mul); /* alpha times mul */

/* a design frame: screen = o + design * s */
typedef struct {
	float ox, oy, sx, sy;
} uin_xf_t;
/* dw x dh stretched over the rectangle (title, pause: sized in %) */
void uin_xf_stretch(uin_xf_t *xf, float dw, float dh, float x, float y, float w, float h);
/* dw x dh scaled uniformly and centred in the rectangle (lobby, hero, HUD) */
void uin_xf_fit(uin_xf_t *xf, float dw, float dh, float x, float y, float w, float h);
/* the transform the primitives use; NULL is the identity. Text positions
 * go through it, glyph sizes don't (faces are baked at screen size). */
void d_xf(const uin_xf_t *xf);
const uin_xf_t *d_get_xf(void);

void d_blend(int blend); /* R2D_ALPHA (the default) or another R2D_* */
void d_fill(float x, float y, float w, float h, uint32_t rgba);
void d_grad_v(float x, float y, float w, float h, uint32_t top, uint32_t bottom);
void d_grad_h(float x, float y, float w, float h, uint32_t left, uint32_t right);
/* a region of the atlas; u0 v0 u1 v1 are fractions of the region */
void d_img(int region, float x, float y, float w, float h, uint32_t rgba);
void d_img_uv(int region, float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t rgba);
/* centred on cx cy, turned deg degrees clockwise (CSS rotate) */
void d_img_rot(int region, float cx, float cy, float w, float h, float deg, uint32_t rgba);
/* one region as a nine-slice: l t r b are its borders in region pixels,
 * drawn at that size; the middle stretches */
void d_nine(int region, float x, float y, float w, float h, float l, float t, float r, float b, uint32_t rgba);
/* an engine image (R_ImageHandle, a levelshot): its own draw call */
void d_pic(int handle, float x, float y, float w, float h, float s0, float t0, float s1, float t1, uint32_t rgba);
/* a ring of segs quads between radii r0 and r1, from the white block */
void d_annulus(float cx, float cy, float r0, float r1, int segs, uint32_t rgba);
/* four corners in order (top left, top right, bottom right, bottom left),
 * from the white block, one colour each: any convex shape */
void d_quad4(const float *xy, const uint32_t *rgba4);

/* text: horizontal | vertical alignment */
enum {
	UIN_LEFT = 0, UIN_CENTER = 1, UIN_RIGHT = 2,
	UIN_TOP = 0 << 2,      /* y is the top of the em box (canvas "top") */
	UIN_MIDDLE = 1 << 2,   /* y is the middle of the em box (canvas "middle") */
	UIN_BASELINE = 2 << 2, /* y is the baseline */
	UIN_BOTTOM = 3 << 2    /* y is the bottom of the em box */
};
/* draws s (UTF-8; code points the face lacks are skipped) and returns its
 * width. Glyphs land on whole pixels unless rotated. The face's outline,
 * if baked, stays black; the colour tints the ink. */
float d_text(int face, float x, float y, const char *s, int align, uint32_t rgba);
/* tracking adds px between letters (CSS letter-spacing); rot turns the
 * line deg degrees clockwise about x y (-90 reads bottom to top) */
float d_text_ex(int face, float x, float y, const char *s, int align, uint32_t rgba, float tracking, float rot);
float d_text_width(int face, const char *s, float tracking);
/* s cut to fit maxw with "..." (canvas names, app.js:457), into out */
const char *d_text_fit(int face, const char *s, float maxw, char *out, size_t outsize);

/* scissor in transformed virtual pixels; both flush. The scissor is the
 * renderer's one: ui_native.c unclips after each draw, which also ends a
 * clip area the client game had set around its drawpic of the HUD. */
void d_clip(float x, float y, float w, float h);
void d_unclip(void);
void d_flush(void);

/* this frame's vertices and draw calls so far, for ui_native_stats */
void d_counters(int *verts, int *calls);
void d_reset_counters(void);

/* --- uin_atlas.c: baked glyphs and images ------------------------------------------ */

typedef struct {
	uint16_t x, y;      /* in its page */
	uint8_t w, h;
	int8_t xoff, yoff;  /* from the pen on the baseline to the bitmap's top left */
	uint16_t adv64;     /* advance in 1/64 px */
	uint8_t page;
	uint8_t face;
	uint16_t cp;
} uin_glyph_t;

typedef struct {
	bool ok;            /* baked (the file decoded) */
	uint8_t page;
	uint16_t x, y, w, h;
	float s0, t0, s1, t1; /* texture coordinates of the region */
} uin_region_t;

typedef struct {
	bool ok;            /* some file of it rendered */
	uint8_t page;
	uint8_t outline;    /* pixels of baked outline around the ink */
	float px;           /* the size it was baked at */
	float ascent, descent; /* em box above and below the baseline, px (descent > 0) */
	float line;         /* line height, px */
} uin_face_t;

/* loads the atlas for the current layout height (cache, else a bake) the
 * first time it is needed or after the height changed; false while there is
 * no renderer. Called by ui_native.c before any drawing. */
bool uin_atlas_ready(void);
/* the layout height H the atlas is for: the virtual height inside the safe
 * inset (432 on the Xbox at 480 lines, 480 without), 0 before loading */
int uin_atlas_height(void);
int uin_atlas_image(int page);         /* R_Draw2DTriangles handle, 0 if none */
void uin_atlas_page_size(int page, int *w, int *h);
void uin_white_uv(int page, float *s, float *t);
const uin_face_t *uin_face(int face);
const uin_glyph_t *uin_glyph(int face, int cp); /* NULL if not baked */
float uin_kern(int face, int a, int b);         /* pair kerning, px */
const uin_region_t *uin_region(int id);

/* ui_native_bake H: bakes the manifest for layout height H and writes the
 * cache ui/native_<H>.txt, _p0.tga, _p1.tga in the writable game dir */
bool uin_atlas_bake_to_cache(int h);
void uin_atlas_invalidate(void); /* reload on next use (ui_font, safe change) */
void uin_atlas_init(void);       /* registers nothing; resets state */
void uin_atlas_shutdown(void);
/* family table (ui_font): the file for a family at the nearest weight */
bool uin_font_add(const char *family, const char *path, int weight);
const char *uin_font_path(const char *family, int weight);

#endif
