/* Bulwark engine - debugging views: r_showclip draws the collision brushes
 * of the world (1: the ones the loader added for props and displacements,
 * 2: every brush), r_debugview places the camera and r_draw2d 0 hides
 * the HUD, menus and console for clean captures.
 * ISC licence, see LICENSE. */
#include "r_local.h"

#include <math.h>
#include <stdio.h>

cvar_t *r_showclip, *r_debugview, *r_draw2d;

#define MAX_WPTS 64

/* the polygon of one brush side: a big square on the plane, cut back by
 * every other side */
static int side_winding(const bspmap_t *m, const cbrush_t *b, int s, float (*out)[3])
{
	const cplane_t *p = &m->planes[m->brushsides[b->firstside + s].plane];
	float up[3] = {0, 0, 1}, right[3], org[3], pts[2][MAX_WPTS][3];
	int n = 4, cur = 0;
	if (fabsf(p->normal[2]) > 0.7f)
		up[0] = 1, up[2] = 0;
	CrossProduct(up, p->normal, right);
	VectorNormalize(right);
	CrossProduct(p->normal, right, up);
	VectorScale(p->normal, p->dist, org);
	for (int i = 0; i < 4; i++) {
		float sr = (i == 0 || i == 3) ? -8192.0f : 8192.0f, su = (i < 2) ? -8192.0f : 8192.0f;
		for (int k = 0; k < 3; k++)
			pts[0][i][k] = org[k] + right[k] * sr + up[k] * su;
	}
	for (int o = 0; o < b->numsides && n >= 3; o++) {
		const cplane_t *c = &m->planes[m->brushsides[b->firstside + o].plane];
		int nn = 0;
		if (o == s)
			continue;
		for (int i = 0; i < n && nn < MAX_WPTS - 1; i++) {
			const float *a = pts[cur][i], *bb = pts[cur][(i + 1) % n];
			float da = DotProduct(a, c->normal) - c->dist, db = DotProduct(bb, c->normal) - c->dist;
			if (da <= 0.01f)
				VectorCopy(a, pts[!cur][nn++]);
			if ((da > 0.01f) != (db > 0.01f) && nn < MAX_WPTS - 1) {
				float t = da / (da - db);
				for (int k = 0; k < 3; k++)
					pts[!cur][nn][k] = a[k] + (bb[k] - a[k]) * t;
				nn++;
			}
		}
		cur = !cur;
		n = nn;
	}
	if (n < 3)
		return 0;
	memcpy(out, pts[cur], sizeof(float) * 3 * n);
	return n;
}

void R_DrawClipBrushes(void)
{
	const bspmap_t *m = cl.world;
	static rvert_t v[MAX_WPTS];
	static uint16_t idx[MAX_WPTS * 3];
	if (!r_showclip || !r_showclip->ivalue || !m || m->type != BSP_Q3 || !m->nummodels)
		return;
	R_UseProgram(PROG_UNLIT);
	qglUniformMatrix4fv(r_progs[PROG_UNLIT].u_mvp, 1, GL_FALSE, r_scene.viewproj);
	qglUniform4f(r_progs[PROG_UNLIT].u_color, 1, 1, 1, 1);
	qglUniform4f(r_progs[PROG_UNLIT].u_tcmod, 0, 0, 1, 1);
	qglUniform1f(r_progs[PROG_UNLIT].u_alphatest, 0);
	GL_BindTexture(0, GL_TEXTURE_2D, r_whiteimage->tex);
	GL_SetBlend(BLEND_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	GL_SetDepth(true, false);
	GL_SetCull(false);
	for (int bi = 0; bi < m->models[0].numbrushes; bi++) {
		const cbrush_t *b = &m->brushes[m->models[0].firstbrush + bi];
		if (!b->added && r_showclip->ivalue < 2)
			continue;
		for (int s = 0; s < b->numsides; s++) {
			float w[MAX_WPTS][3], shade;
			const cplane_t *p = &m->planes[m->brushsides[b->firstside + s].plane];
			int n = side_winding(m, b, s, w), ni = 0;
			if (!n)
				continue;
			/* faces lit a little by their facing, so boxes read as solids */
			shade = 0.6f + 0.4f * fabsf(p->normal[2]) * 0.5f + 0.2f * p->normal[0];
			for (int i = 0; i < n; i++) {
				memset(&v[i], 0, sizeof(v[i]));
				VectorCopy(w[i], v[i].xyz);
				v[i].color[0] = (uint8_t)BW_MIN(255.0f, (b->added ? 255 : 60) * shade);
				v[i].color[1] = (uint8_t)BW_MIN(255.0f, (b->added ? 120 : 140) * shade);
				v[i].color[2] = (uint8_t)BW_MIN(255.0f, (b->added ? 20 : 255) * shade);
				v[i].color[3] = 150;
			}
			for (int i = 1; i + 1 < n; i++) {
				idx[ni++] = 0;
				idx[ni++] = (uint16_t)i;
				idx[ni++] = (uint16_t)(i + 1);
			}
			R_StreamDraw(GL_TRIANGLES, v, n, idx, ni);
		}
	}
	GL_SetDepth(true, true);
	GL_SetCull(true);
}

/* "x y z pitch yaw": the view is put there, whatever the game says */
void R_DebugView(void)
{
	float o[3], a[2];
	if (!r_debugview || !r_debugview->string[0])
		return;
	if (sscanf(r_debugview->string, "%f %f %f %f %f", &o[0], &o[1], &o[2], &a[0], &a[1]) == 5) {
		VectorCopy(o, r_scene.origin);
		r_scene.angles[0] = a[0];
		r_scene.angles[1] = a[1];
		r_scene.angles[2] = 0;
	}
}
