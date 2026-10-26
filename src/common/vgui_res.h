/* Bulwark engine - VGUI layout (.res) and scheme files on top of the
 * KeyValues parser: colours, scheme fonts and panel positions. The rules and
 * where each comes from are in docs/spec/vgui-res.md. ISC licence, see
 * LICENSE. Nothing here draws; it turns files into numbers a HUD or menu
 * renderer can use. */
#ifndef BW_VGUI_RES_H
#define BW_VGUI_RES_H

#include "keyvalues.h"

enum {
	VGUI_FONT_ANTIALIAS = 1,
	VGUI_FONT_DROPSHADOW = 2,
	VGUI_FONT_UNDERLINE = 4,
	VGUI_FONT_ITALIC = 8,
	VGUI_FONT_OUTLINE = 16,
	VGUI_FONT_SYMBOL = 32,
	VGUI_FONT_ADDITIVE = 64,
};

typedef struct {
	const char *name; /* face name */
	int tall, weight;
	int yres_min, yres_max; /* 0, 0 when the definition has no "yres" */
	unsigned flags;
} vgui_fontdef_t;

typedef struct {
	const char *name;
	int numdefs;
	vgui_fontdef_t *defs; /* in file order */
} vgui_font_t;

typedef struct vgui_scheme_s {
	kv_t *kv;              /* the parsed file, owned */
	const kv_t *colors;    /* "Colors" block, may be NULL */
	const kv_t *base;      /* "BaseSettings" block, may be NULL */
	const kv_t *borders;   /* "Borders" block, may be NULL */
	const kv_t *fontfiles; /* "CustomFontFiles" block, may be NULL */
	int numfonts;
	vgui_font_t *fonts;
	/* consulted for colour names this scheme does not define; set by the
	 * caller, not owned (a design choice, see the spec) */
	const struct vgui_scheme_s *fallback;
} vgui_scheme_t;

typedef struct {
	const kv_t *kv; /* the panel's block, for any other key */
	const char *name;    /* "fieldName", or the block's key */
	const char *control; /* "ControlName", or "" */
	int x, y, w, h, z;
	bool visible, enabled;
	bool badcoord; /* a position or size that did not parse; it was taken as 0 */
} vgui_panel_t;

/* "R G B" or "R G B A" (A defaults to 255); false unless 3 or 4 integers */
bool VGUI_ParseColor(const char *s, uint8_t out[4]);

/* takes ownership of nothing: the buffer is parsed and can be freed after */
bool VGUI_LoadScheme(vgui_scheme_t *s, const uint8_t *buf, size_t len, kv_report_t *rep);
void VGUI_FreeScheme(vgui_scheme_t *s);
/* a literal colour, a "Colors" entry (a '/' path reaches nested blocks), or
 * a "BaseSettings" entry naming either, then the same in the fallback
 * schemes; false if it cannot be resolved */
bool VGUI_SchemeColor(const vgui_scheme_t *s, const char *ref, uint8_t out[4]);
const vgui_font_t *VGUI_SchemeFont(const vgui_scheme_t *s, const char *name);
/* the definition a screen of this height uses, see the spec for the rule */
const vgui_fontdef_t *VGUI_PickFontDef(const vgui_font_t *f, int screenh);

/* one coordinate: "N", "-N", "rN" (from the far edge), "cN" or "c-N" (from
 * the centre); parent is the containing size, scale multiplies N. Returns
 * false (and 0) for anything else. */
bool VGUI_ResolveCoord(const char *s, int parent, float scale, int *out);
/* every block inside a layout root, positioned against a parent of pw x ph;
 * returns how many there are, filling at most max */
int VGUI_LayoutPanels(const kv_t *root, int pw, int ph, float scale, vgui_panel_t *out, int max);

#endif
