/* Bulwark engine - collision for Source props. A Source map does not carry
 * its props' collision: it lives in each model's .phy file. When that file
 * can be read, its convex pieces become brushes (exact). Otherwise boxes
 * from a prop clip table, or the .mdl's hull box, stand in (approximate).
 * The props themselves need not be drawable. See docs/spec/vbsp.md.
 * ISC licence, see LICENSE. */
#include "bsp_source.h"
#include "../common/fs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int32_t rd32(const uint8_t *p) { return (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }
static uint16_t rdu16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static float rdf(const uint8_t *p)
{
	int32_t i = rd32(p);
	float f;
	memcpy(&f, &i, 4);
	return f;
}

/* --- placing a model ------------------------------------------------------ */

typedef struct {
	float origin[3];
	float axis[3][3]; /* forward, left, up */
	int model;        /* brush model the hull goes into */
} placement_t;

static void place(placement_t *pl, const float *origin, const float *angles, const float *modelorigin, int model)
{
	float right[3];
	AngleVectors(angles, pl->axis[0], right, pl->axis[2]);
	VectorScale(right, -1, pl->axis[1]);
	for (int k = 0; k < 3; k++)
		pl->origin[k] = origin[k] - (modelorigin ? modelorigin[k] : 0);
	pl->model = model;
}

static void transform(const placement_t *pl, const float *in, float *out)
{
	for (int k = 0; k < 3; k++)
		out[k] = pl->origin[k] + pl->axis[0][k] * in[0] + pl->axis[1][k] * in[1] + pl->axis[2][k] * in[2];
}

static void add_box(srchulls_t *out, const placement_t *pl, const float *mins, const float *maxs)
{
	float pts[8][3];
	for (int i = 0; i < 8; i++) {
		float c[3] = {(i & 1) ? maxs[0] : mins[0], (i & 2) ? maxs[1] : mins[1], (i & 4) ? maxs[2] : mins[2]};
		transform(pl, c, pts[i]);
	}
	SrcHull_Add(out, pl->model, CONTENTS_SOLID, (const float(*)[3])pts, 8);
}

/* --- .phy files ------------------------------------------------------------ */

/* A .phy holds a 16-byte header (header size, id, solid count, checksum),
 * then per solid its size and an IVP compact surface, optionally behind a
 * 28-byte "VPHY" header. The surface's ledge tree leads to ledges: convex
 * pieces whose triangles index a shared table of points. IVP works in
 * metres with y pointing down: Source x = x, y = z, z = -y, times 1/0.0254. */
#define IVP_TO_HL (1.0f / 0.0254f)

typedef struct {
	const uint8_t *base, *end;
	const placement_t *pl;
	srchulls_t *out;
	int pieces;
} phywalk_t;

static void phy_ledge(phywalk_t *w, const uint8_t *ledge)
{
	int pointofs, ntris;
	float pts[HULL_MAXPTS_PHY][3];
	int idx[HULL_MAXPTS_PHY], n = 0;
	if (ledge < w->base || ledge + 16 > w->end)
		return;
	pointofs = rd32(ledge);
	ntris = (int16_t)rdu16(ledge + 12);
	if (ntris <= 0 || ledge + 16 + (size_t)ntris * 16 > w->end)
		return;
	for (int t = 0; t < ntris; t++)
		for (int e = 0; e < 3; e++) {
			const uint8_t *edge = ledge + 16 + t * 16 + 4 + e * 4;
			int pi = rdu16(edge), k;
			for (k = 0; k < n; k++)
				if (idx[k] == pi)
					break;
			if (k == n && n < HULL_MAXPTS_PHY)
				idx[n++] = pi;
		}
	for (int i = 0; i < n; i++) {
		const uint8_t *p = ledge + pointofs + (size_t)idx[i] * 16;
		float local[3];
		if (p < w->base || p + 12 > w->end)
			return;
		local[0] = rdf(p) * IVP_TO_HL;
		local[1] = rdf(p + 8) * IVP_TO_HL;
		local[2] = -rdf(p + 4) * IVP_TO_HL;
		transform(w->pl, local, pts[i]);
	}
	SrcHull_Add(w->out, w->pl->model, CONTENTS_SOLID, (const float(*)[3])pts, n);
	w->pieces++;
}

/* ledge tree node: right child offset (0 for a leaf), ledge offset, centre,
 * radius, box sizes; 28 bytes, the left child follows directly */
static void phy_node(phywalk_t *w, const uint8_t *node, int depth)
{
	int right, ledge;
	if (depth > 64 || node < w->base || node + 28 > w->end)
		return;
	right = rd32(node);
	ledge = rd32(node + 4);
	if (!right) {
		phy_ledge(w, node + ledge);
		return;
	}
	phy_node(w, node + 28, depth + 1);
	phy_node(w, node + right, depth + 1);
}

static int phy_hulls(const char *model, const placement_t *pl, srchulls_t *out)
{
	char name[160];
	size_t len;
	const char *dot = strrchr(model, '.');
	uint8_t *d;
	int nsolids, pieces = 0;
	size_t p;
	snprintf(name, sizeof(name), "%.*s.phy", dot ? (int)(dot - model) : (int)strlen(model), model);
	d = (uint8_t *)FS_LoadFile(name, &len);
	if (!d)
		return -1;
	if (len < 16 || rd32(d) != 16)
		goto done;
	nsolids = rd32(d + 8);
	p = 16;
	for (int s = 0; s < nsolids && p + 4 <= len; s++) {
		int ssize = rd32(d + p);
		const uint8_t *surf = d + p + 4, *send = surf + ssize;
		phywalk_t w;
		if (ssize <= 0 || p + 4 + (size_t)ssize > len)
			break;
		if (!memcmp(surf, "VPHY", 4))
			surf += 28;
		if (surf + 48 > send || memcmp(surf + 44, "IVPS", 4))
			break;
		w.base = surf;
		w.end = send;
		w.pl = pl;
		w.out = out;
		w.pieces = 0;
		phy_node(&w, surf + rd32(surf + 32), 0);
		pieces += w.pieces;
		p += 4 + (size_t)ssize;
	}
done:
	Mem_Free(d);
	return pieces;
}

/* --- prop clip tables -------------------------------------------------------- */

/* Text files of lines: "model" box minx miny minz maxx maxy maxz (repeat
 * the line for more boxes), or "model" none for no collision. The map's own
 * maps/<map>.propclip wins over scripts/propclip.txt for a model it names. */
typedef struct {
	char model[128];
	bool none;
	float mins[3], maxs[3];
	int file; /* 0 the map's, 1 the shared one */
} clipentry_t;

typedef struct {
	clipentry_t *e;
	int num, cap;
} cliptable_t;

static void load_table(cliptable_t *t, const char *path, int file)
{
	char *text = FS_LoadFile(path, NULL), tok[128];
	const char *s = text;
	if (!text)
		return;
	while ((s = COM_Parse(s, tok, sizeof(tok))) != NULL) {
		clipentry_t e;
		memset(&e, 0, sizeof(e));
		Str_Copy(e.model, tok, sizeof(e.model));
		e.file = file;
		if (!(s = COM_Parse(s, tok, sizeof(tok))))
			break;
		if (!Str_ICmp(tok, "none")) {
			e.none = true;
		} else if (!Str_ICmp(tok, "box")) {
			bool ok = true;
			for (int k = 0; k < 6 && ok; k++) {
				if (!(s = COM_Parse(s, tok, sizeof(tok))))
					ok = false;
				else if (k < 3)
					e.mins[k] = (float)atof(tok);
				else
					e.maxs[k - 3] = (float)atof(tok);
			}
			if (!ok)
				break;
		} else {
			Con_Printf("%s: unknown prop clip kind \"%s\"\n", path, tok);
			continue;
		}
		if (t->num == t->cap) {
			t->cap = t->cap ? t->cap * 2 : 64;
			t->e = Mem_Realloc(t->e, sizeof(clipentry_t) * t->cap);
		}
		t->e[t->num++] = e;
	}
	Mem_Free(text);
}

/* boxes for a model from the table: -1 not listed, else the box count */
static int table_hulls(const cliptable_t *t, const char *model, const placement_t *pl, srchulls_t *out)
{
	int best = 2, n = 0;
	for (int i = 0; i < t->num; i++)
		if (!Str_ICmp(t->e[i].model, model))
			best = BW_MIN(best, t->e[i].file);
	if (best == 2)
		return -1;
	for (int i = 0; i < t->num; i++) {
		const clipentry_t *e = &t->e[i];
		if (e->file != best || Str_ICmp(e->model, model) || e->none)
			continue;
		add_box(out, pl, e->mins, e->maxs);
		n++;
	}
	return n;
}

/* the hull box in a studio model header (bytes 104 and 116) */
static bool mdl_hull(const char *model, const placement_t *pl, srchulls_t *out)
{
	size_t len;
	uint8_t *d = (uint8_t *)FS_LoadFile(model, &len);
	float mins[3], maxs[3];
	bool ok = false;
	if (d && len >= 128 && !memcmp(d, "IDST", 4)) {
		for (int k = 0; k < 3; k++) {
			mins[k] = rdf(d + 104 + k * 4);
			maxs[k] = rdf(d + 116 + k * 4);
		}
		if (maxs[0] > mins[0] && maxs[1] > mins[1] && maxs[2] > mins[2]) {
			add_box(out, pl, mins, maxs);
			ok = true;
		}
	}
	Mem_Free(d);
	return ok;
}

/* --- entities ---------------------------------------------------------------- */

#define MAX_KEYS 64
typedef struct {
	char key[MAX_KEYS][64];
	char value[MAX_KEYS][256];
	int num;
} entkeys_t;

static const char *ent_get(const entkeys_t *e, const char *key)
{
	for (int i = 0; i < e->num; i++)
		if (!Str_ICmp(e->key[i], key))
			return e->value[i];
	return "";
}

/* the next entity's keys; NULL at the end */
static const char *next_entity(const char *s, entkeys_t *e)
{
	char tok[256];
	e->num = 0;
	if (!(s = COM_Parse(s, tok, sizeof(tok))) || strcmp(tok, "{"))
		return NULL;
	while ((s = COM_Parse(s, tok, sizeof(tok))) != NULL && strcmp(tok, "}")) {
		char val[256];
		if (!(s = COM_Parse(s, val, sizeof(val))))
			return NULL;
		if (e->num < MAX_KEYS) {
			Str_Copy(e->key[e->num], tok, sizeof(e->key[0]));
			Str_Copy(e->value[e->num], val, sizeof(e->value[0]));
			e->num++;
		}
	}
	return s;
}

static void parse_vec(const char *s, float *v)
{
	v[0] = v[1] = v[2] = 0;
	sscanf(s, "%f %f %f", &v[0], &v[1], &v[2]);
}

/* brush entities by name, for props that ride on them */
typedef struct {
	char name[64];
	int model;
	float origin[3];
} brushent_t;

typedef struct {
	char prop[64], other[64];
} link_t;

static bool is_prop_class(const char *c)
{
	return !Str_ICmp(c, "prop_dynamic") || !Str_ICmp(c, "prop_dynamic_override") || !Str_ICmp(c, "prop_physics") ||
	       !Str_ICmp(c, "prop_physics_override") || !Str_ICmp(c, "prop_physics_multiplayer") ||
	       !Str_ICmp(c, "prop_static") || !Str_ICmp(c, "prop_door_rotating");
}

/* --- putting it together ------------------------------------------------------- */

typedef struct {
	int exact, approx, none, skipped;
} propstats_t;

static void prop_hulls(const char *what, const char *model, const placement_t *pl, const cliptable_t *table,
                       srchulls_t *out, propstats_t *st)
{
	int n;
	const char *how;
	if ((n = phy_hulls(model, pl, out)) > 0) {
		st->exact++;
		how = "exact (.phy)";
	} else if ((n = table_hulls(table, model, pl, out)) >= 0) {
		if (n) {
			st->approx++;
			how = "approximate (prop clip table)";
		} else {
			st->skipped++;
			how = "none (prop clip table says none)";
		}
	} else if (mdl_hull(model, pl, out)) {
		st->approx++;
		how = "approximate (.mdl hull box)";
	} else {
		st->none++;
		how = "MISSING: no .phy, .mdl or prop clip entry";
	}
	if (developer || !strncmp(how, "MISSING", 7))
		Con_Printf("  %s %s at %.0f %.0f %.0f: %s\n", what, model, pl->origin[0], pl->origin[1], pl->origin[2], how);
}

void SrcProps_Hulls(const vbsp_t *v, bspmap_t *m, srchulls_t *out)
{
	vbsp_staticprops_t sp;
	cliptable_t table = {0};
	propstats_t st = {0};
	brushent_t *bents = NULL;
	link_t *links = NULL;
	int nbents = 0, nlinks = 0;
	entkeys_t *e = Mem_Alloc(sizeof(*e));
	const char *s;
	char path[160], base[64];

	/* the tables: maps/<map>.propclip, then scripts/propclip.txt */
	{
		const char *slash = strrchr(m->name, '/'), *n = slash ? slash + 1 : m->name, *dot = strrchr(n, '.');
		snprintf(base, sizeof(base), "%.*s", dot ? (int)(dot - n) : (int)strlen(n), n);
		snprintf(path, sizeof(path), "maps/%s.propclip", base);
		load_table(&table, path, 0);
		load_table(&table, "scripts/propclip.txt", 1);
	}
	if (developer)
		Con_Printf("%s: prop collision\n", base);

	if (VBSP_StaticProps(v, &sp)) {
		for (int i = 0; i < sp.numprops; i++) {
			const vbsp_staticprop_t *p = &sp.props[i];
			placement_t pl;
			char what[32];
			if (!p->solid || p->model < 0)
				continue;
			place(&pl, p->origin, p->angles, NULL, 0);
			snprintf(what, sizeof(what), "static prop %d", i);
			prop_hulls(what, sp.names[p->model], &pl, &table, out, &st);
		}
		VBSP_FreeStaticProps(&sp);
	}

	/* brush entities and what is tied to them */
	for (s = m->entities; (s = next_entity(s, e)) != NULL;) {
		const char *mdl = ent_get(e, "model"), *cls = ent_get(e, "classname");
		if (mdl[0] == '*' && ent_get(e, "targetname")[0]) {
			bents = Mem_Realloc(bents, sizeof(brushent_t) * (nbents + 1));
			Str_Copy(bents[nbents].name, ent_get(e, "targetname"), sizeof(bents[0].name));
			bents[nbents].model = atoi(mdl + 1);
			parse_vec(ent_get(e, "origin"), bents[nbents].origin);
			nbents++;
		}
		if (!Str_ICmp(cls, "phys_constraint") || !Str_ICmp(cls, "phys_ballsocket") || !Str_ICmp(cls, "phys_hinge") ||
		    !Str_ICmp(cls, "phys_lengthconstraint")) {
			links = Mem_Realloc(links, sizeof(link_t) * (nlinks + 2));
			Str_Copy(links[nlinks].prop, ent_get(e, "attach2"), sizeof(links[0].prop));
			Str_Copy(links[nlinks].other, ent_get(e, "attach1"), sizeof(links[0].other));
			Str_Copy(links[nlinks + 1].prop, ent_get(e, "attach1"), sizeof(links[0].prop));
			Str_Copy(links[nlinks + 1].other, ent_get(e, "attach2"), sizeof(links[0].other));
			nlinks += 2;
		}
	}

	for (s = m->entities; (s = next_entity(s, e)) != NULL;) {
		const char *cls = ent_get(e, "classname"), *mdl = ent_get(e, "model"), *solid = ent_get(e, "solid");
		const char *parent = ent_get(e, "parentname"), *name = ent_get(e, "targetname");
		float origin[3], angles[3];
		placement_t pl;
		const brushent_t *rides = NULL;
		int spawnflags = atoi(ent_get(e, "spawnflags"));
		bool physics = !Str_ICmp(cls, "prop_physics") || !Str_ICmp(cls, "prop_physics_override") ||
		               !Str_ICmp(cls, "prop_physics_multiplayer");
		if (!is_prop_class(cls) || !mdl[0] || mdl[0] == '*')
			continue;
		/* prop_dynamic and prop_static are solid (vphysics) unless "solid"
		 * says 0; physics props always are, except debris (spawnflag 4),
		 * which players walk through */
		if (physics ? (spawnflags & 4) != 0 : (solid[0] && atoi(solid) == 0))
			continue;
		parse_vec(ent_get(e, "origin"), origin);
		parse_vec(ent_get(e, "angles"), angles);
		/* riding a brush entity: through its parent, or a constraint */
		for (int i = 0; i < nbents && !rides; i++)
			if (parent[0] && !Str_ICmp(bents[i].name, parent))
				rides = &bents[i];
		for (int l = 0; l < nlinks && !rides && name[0]; l++)
			if (!Str_ICmp(links[l].prop, name))
				for (int i = 0; i < nbents && !rides; i++)
					if (!Str_ICmp(bents[i].name, links[l].other))
						rides = &bents[i];
		if (rides && (rides->model <= 0 || rides->model >= m->nummodels))
			rides = NULL;
		place(&pl, origin, angles, rides ? rides->origin : NULL, rides ? rides->model : 0);
		prop_hulls(cls, mdl, &pl, &table, out, &st);
	}

	Con_Printf("%s: prop collision: %d exact, %d approximate, %d listed as none, %d missing\n", base, st.exact,
	           st.approx, st.skipped, st.none);
	Mem_Free(table.e);
	Mem_Free(bents);
	Mem_Free(links);
	Mem_Free(e);
}
