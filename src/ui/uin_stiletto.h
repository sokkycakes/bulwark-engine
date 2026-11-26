/* Bulwark engine - native UI: Stiletto's design constants and the asset
 * manifest the atlas is baked from. ISC licence, see LICENSE.
 *
 * Every number here is in its page's own design frame and names the file
 * and line of Stiletto's page (base/data/web/...) or of Bulwark's Panorama
 * port (data/panorama/...) it comes from. Nothing of Stiletto's (fonts, art)
 * is in Bulwark: the manifest names game files, read at bake time.
 *
 * The manifest is two X-macro lists. Each screen's owner keeps its own
 * section; adding a line changes the manifest hash, so caches baked from
 * an older list are rebaked (PC) or ignored for a runtime bake (Xbox).
 *
 * Deliberate deviations from the pages are listed at the end. */
#ifndef BW_UIN_STILETTO_H
#define BW_UIN_STILETTO_H

/* --- faces -----------------------------------------------------------------------------
 * X(id, family, weight, fallback path, px, floor px, flags, page, chars)
 *
 *   family, weight  looked up in the ui_font table (panorama/webcore_fonts.cfg,
 *                   the same lines the Panorama build reads); the fallback
 *                   path is used when nothing registered the family.
 *   px              with UF_HUD: the size in the 1440x1080 HUD canvas, baked
 *                   at px * H / 1080 for the layout height H; otherwise the
 *                   baked size in virtual pixels.
 *   floor px        the smallest size baked (legibility floor), 0 for none.
 *   flags           UF_* below.
 *   page            0: HUD and shared, 1: menus.
 *   chars           the characters baked, UIN_ASCII for 32-126.
 *
 * Pixel fonts (UF_PIXEL) sit on an 8 px grid (04b09: upem 1200 in steps of
 * 150; standard_07_57 and hooge_05_57: upem 1000 in steps of 125), so they
 * bake only at multiples of 8 with alpha thresholded, and stay crisp. */
enum {
	UF_HUD = 1,     /* px is in the 1080-line HUD canvas: scaled by H / 1080 */
	UF_PIXEL = 2,   /* a pixel font: snapped to a multiple of 8 px, hard edges */
	UF_OUTLINE = 4, /* a 1 px black outline at alpha 0.5 baked around the ink */
	UF_HEAVY = 8,   /* a big file (Iosevka, 1.3 MB): a runtime bake on the Xbox
	                   uses UIN_HEAVY_SUBSTITUTE instead of reading it */
	UF_ROT = 16     /* drawn rotated or scaled: 2 px of padding, not 1 */
};

#define UIN_ASCII NULL
#define UIN_DIGITS "0123456789"
#define UIN_HUDFONTS "data/web/hud/assets/fonts/"
/* the face a runtime bake on the Xbox reads for UF_HEAVY faces */
#define UIN_HEAVY_SUBSTITUTE "Trade Gothic Next LT Pro BdCn", 400, UIN_HUDFONTS "TradeGothicNextLTProBdCn.ttf"
/* when a face's file is missing: Bulwark's own OFL pixel font (data/fonts) */
#define UIN_FALLBACK_FONT "fonts/tallpixelfont.ttf"

#define UIN_FACES(X) \
	/* HUD (uin_hud.c): hud/app.js sizes at 1080 lines, design 3.2 */ \
	X(UIN_F_SCORE, "Helvetica Neue 97 Black Condensed", 400, UIN_HUDFONTS "HelveticaNeue97BlackCondensed.otf", \
	  75.965f, 0, UF_HUD | UF_OUTLINE, 0, UIN_DIGITS "-")                         /* app.js:432 */ \
	X(UIN_F_EXCOUNT, "Helvetica Neue 97 Black Condensed Oblique", 400, \
	  UIN_HUDFONTS "HelveticaNeue97BlackCondensedOblique.otf", 96, 0, UF_HUD | UF_OUTLINE, 0, UIN_DIGITS) /* app.js:583 */ \
	X(UIN_F_MAX, "Helvetica Neue 97 Black Condensed Oblique", 400, \
	  UIN_HUDFONTS "HelveticaNeue97BlackCondensedOblique.otf", 36, 0, UF_HUD | UF_OUTLINE, 0, "MAX") /* app.js:590 */ \
	X(UIN_F_SPEED, "Iosevka Charon Mono", 500, UIN_HUDFONTS "IosevkaCharonMono-Medium.ttf", 32, 0, \
	  UF_HUD | UF_OUTLINE | UF_HEAVY, 0, UIN_DIGITS)                              /* app.js:368 */ \
	X(UIN_F_RELOAD, "Iosevka Charon Mono", 700, UIN_HUDFONTS "IosevkaCharonMono-Bold.ttf", 20, 0, \
	  UF_HUD | UF_HEAVY, 0, UIN_DIGITS ".")                                       /* app.js:560 */ \
	X(UIN_F_TIMER, "Trade Gothic Next LT Pro BdCn", 400, UIN_HUDFONTS "TradeGothicNextLTProBdCn.ttf", 28, 0, \
	  UF_HUD, 0, UIN_DIGITS ":")                                                  /* app.js:424 */ \
	X(UIN_F_NAME, "Trade Gothic Next LT Pro BdCn", 400, UIN_HUDFONTS "TradeGothicNextLTProBdCn.ttf", 14.813f, 10, \
	  UF_HUD, 0, UIN_ASCII)                                                       /* app.js:457 */ \
	X(UIN_F_CLIP, "04b09", 400, UIN_HUDFONTS "04b09.ttf", 33.3f, 0, UF_HUD | UF_PIXEL, 0, UIN_DIGITS "|") /* app.js:545 */ \
	X(UIN_F_CLIPSIZE, "04b09", 400, UIN_HUDFONTS "04b09.ttf", 16, 0, UF_HUD | UF_PIXEL, 0, UIN_DIGITS) /* app.js:551 */ \
	X(UIN_F_COOLDOWN, "Helvetica Neue 97 Black Condensed", 400, UIN_HUDFONTS "HelveticaNeue97BlackCondensed.otf", \
	  11, 7, UF_HUD, 0, UIN_DIGITS ".")                                           /* app.js:633 */ \
	/* menus (uin_title.c, uin_lobby.c, uin_pause.c, uin_hero.c, uin_misc.c): design 5.2 page 1 */ \
	X(UIN_F_STD8, "standard_07_57", 400, "data/web/ql-menu/assets/standard_07_57.ttf", 8, 0, UF_PIXEL, 1, UIN_ASCII) \
	X(UIN_F_STD16, "standard_07_57", 400, "data/web/ql-menu/assets/standard_07_57.ttf", 16, 0, UF_PIXEL, 1, UIN_ASCII) \
	X(UIN_F_HOOGE8, "hooge_05_57", 400, "data/web/title-menu-v2/assets/hooge_05_57.ttf", 8, 0, UF_PIXEL, 1, UIN_ASCII) \
	X(UIN_F_TG15, "Trade Gothic Next LT Pro BdCn", 400, UIN_HUDFONTS "TradeGothicNextLTProBdCn.ttf", 15, 0, 0, 1, \
	  UIN_ASCII)                                                                  /* pause-menu.css:11 */ \
	X(UIN_F_PAUSE, "Trade Gothic Next LT Pro BdCn", 400, UIN_HUDFONTS "TradeGothicNextLTProBdCn.ttf", 32, 0, UF_ROT, 1, \
	  "PAUSE")                                                                    /* pause banner, -90 deg */ \
	X(UIN_F_INTER10, "Inter", 400, "data/web/lobby-menu/assets/fonts/Inter-Regular.ttf", 10, 0, 0, 1, UIN_ASCII) \
	X(UIN_F_INTERB10, "Inter", 700, "data/web/lobby-menu/assets/fonts/Inter-Bold.ttf", 10, 0, 0, 1, UIN_ASCII) \
	X(UIN_F_INTERB26, "Inter", 700, "data/web/lobby-menu/assets/fonts/Inter-Bold.ttf", 26, 0, 0, 1, \
	  "ABCDEFGHIJKLMNOPQRSTUVWXYZ" UIN_DIGITS "_")                                /* lobby map name */

/* --- images ----------------------------------------------------------------------------
 * X(id, path, w, h, flags, page)
 *
 *   path   a game file (PNG, SVG with masks, ...), or a "*name" made by the
 *          baker itself: *disc (an antialiased white disc). Every page also
 *          gets a white block at its origin for fills (uin_white_uv).
 *   w, h   with UF_HUD the size in the 1080-line HUD canvas, scaled by H/1080
 *          and rounded; otherwise the baked size in virtual pixels. Images
 *          are box-filtered down with premultiplied averaging; SVGs are
 *          drawn at 4x the baked size first (R_DecodeSVGSized).
 *   flags  UF_HUD, UF_ROT (2 px padding, for rotated or stretched use)
 *
 * Every image region's padding repeats its edge pixels, so stretching a
 * region (nine-slice edges) never pulls in its neighbour or transparency. */
#define UIN_HUDART "data/web/hud/assets/"
#define UIN_QLART "data/web/ql-menu/assets/score/"

#define UIN_IMAGES(X) \
	X(UIN_R_DISC, "*disc", 16, 16, 0, 0)                  /* button prompts, tinted per button */ \
	/* HUD (uin_hud.c): design sizes at 1080 lines, design 3.2 */ \
	X(UIN_R_TIMEPLATE, UIN_HUDART "time_indicator.svg", 248, 164, UF_HUD, 0) /* app.js:416 */ \
	X(UIN_R_GAUGE, UIN_HUDART "union.svg", 482, 31.595f, UF_HUD, 0)       /* app.js:578, masked */ \
	X(UIN_R_EXPLATE, UIN_HUDART "ex_indicator.svg", 86, 33, UF_HUD, 0)    /* app.js:575 */ \
	X(UIN_R_HEART, UIN_HUDART "hp_vector.svg", 35, 35, UF_HUD, 0)         /* app.js:505, masked */ \
	X(UIN_R_HEARTLOW, UIN_HUDART "hpLow.png", 35, 35, UF_HUD, 0)          /* app.js:506 */ \
	X(UIN_R_HEARTDEAD, UIN_HUDART "hp_dead_vector.svg", 50.865f, 50.865f, UF_HUD, 0) /* app.js:509, masked */ \
	X(UIN_R_MINIPIP, UIN_HUDART "abilitypip1.png", 20.574f, 20.574f, UF_HUD, 0) /* app.js:480 */ \
	X(UIN_R_PIP, UIN_HUDART "abilitypip1.png", 17.37f, 17.37f, UF_HUD | UF_ROT, 0) /* app.js:528, -45 deg */ \
	X(UIN_R_CD0, UIN_HUDART "circles_ext1.svg", 20, 20, UF_HUD, 0)        /* app.js:604 */ \
	X(UIN_R_CD1, UIN_HUDART "deployed_code.svg", 20, 20, UF_HUD, 0) \
	X(UIN_R_CD2, UIN_HUDART "deblur.svg", 20, 20, UF_HUD, 0) \
	/* hero picker (uin_hero.c): ql-menu/app.js:389-401 scoreInset, 20/10/10 */ \
	X(UIN_R_SCORE_TL, UIN_QLART "scoretl3.png", 20, 20, UF_ROT, 1) \
	X(UIN_R_SCORE_T, UIN_QLART "scoretm3.png", 16, 20, UF_ROT, 1) \
	X(UIN_R_SCORE_TR, UIN_QLART "scoretr3.png", 20, 20, UF_ROT, 1) \
	X(UIN_R_SCORE_L, UIN_QLART "scorel.png", 10, 16, UF_ROT, 1) \
	X(UIN_R_SCORE_M, UIN_QLART "scorem.png", 16, 16, UF_ROT, 1) \
	X(UIN_R_SCORE_R, UIN_QLART "scorer.png", 10, 16, UF_ROT, 1) \
	X(UIN_R_SCORE_BL, UIN_QLART "scorebl.png", 10, 10, UF_ROT, 1) \
	X(UIN_R_SCORE_B, UIN_QLART "scoreb.png", 16, 10, UF_ROT, 1) \
	X(UIN_R_SCORE_BR, UIN_QLART "scorebr.png", 10, 10, UF_ROT, 1) \
	X(UIN_R_NAVRIGHT, UIN_QLART "navright.png", 80, 20, 0, 1)            /* app.js:1477 chips */ \
	X(UIN_R_NAVRIGHTY, UIN_QLART "navrighty.png", 80, 20, 0, 1) \
	X(UIN_R_BIGRED, UIN_QLART "bigred.png", 103, 24, 0, 1)               /* app.js:1478 join */ \
	/* title (uin_title.c) */ \
	X(UIN_R_LOGO, "data/web/title-menu-v2/assets/tomboy-logo-sm.png", 72, 26, 0, 1) /* title-menu-v2.css logo */ \
	/* one wordmark cell of the tiled background at 640x480: title-menu-v2.js:268-331 \
	   (tile 428.224 x 70.272 of the 1365 x 768 frame, the 429 x 71 SVG fitted inside) */ \
	X(UIN_R_LOGOTILE, "data/web/title-menu-v2/assets/logotile.svg", 265, 44, UF_ROT, 1) \
	/* lobby (uin_lobby.c): lobby-menu.css:187-193 lock art, 183 px at 1080 lines, -28.42 deg */ \
	X(UIN_R_GUESTLOCK, "data/web/lobby-menu/assets/guest-lock.svg", 81, 103, UF_ROT, 1)

/* --- shared design constants ------------------------------------------------------- */

/* button prompt discs (design 6.0 footer) */
#define UIN_COL_PAD_A 0x5fb236ffu
#define UIN_COL_PAD_B 0xd83a2affu
#define UIN_COL_PAD_X 0x3a7bd8ffu
#define UIN_COL_PAD_Y 0xe8c22affu
#define UIN_COL_FOOTER_TEXT 0xbfbfbfffu
/* the footer's right edge and baseline at 640x480: action-safe */
#define UIN_FOOTER_RIGHT 576
#define UIN_FOOTER_BASELINE 448

/* ql-menu/app.js FOCUS, the menus' highlight */
#define UIN_COL_FOCUS 0xffbf00ffu
/* pause-menu.css accent, also the error dialog's edge and the loading bar */
#define UIN_COL_ACCENT 0xb70043ffu

/* design frames (design 6.0): title and pause stretch, lobby and hero fit */
#define UIN_FRAME_TITLE_W 1024
#define UIN_FRAME_TITLE_H 768
#define UIN_FRAME_PAUSE_W 1280
#define UIN_FRAME_PAUSE_H 720
#define UIN_FRAME_LOBBY_W 960
#define UIN_FRAME_LOBBY_H 720
#define UIN_FRAME_HERO_W 640
#define UIN_FRAME_HERO_H 480
#define UIN_FRAME_HUD_W 1440 /* hud/app.js:9-10 */
#define UIN_FRAME_HUD_H 1080

/* --- deliberate deviations ----------------------------------------------------------
 * - Pixel faces snap to 8 or 16 px (04b09 33.3 -> 16 at H 432, hud 16 -> 8;
 *   the title's 10 px standard_07_57 -> 8). Other sizes blur.
 * - TG names have a 10 px floor (5.9 px at H 432 is unreadable).
 * - The prompt discs are one white disc tinted per button, with the letter
 *   drawn over it in UIN_F_STD8, not four baked discs.
 * - The ring is two procedural annuli (design 3.2 #1), not the 2048^2 hudring.
 * - A runtime bake on the Xbox draws the Iosevka faces in Trade Gothic. */

#endif
