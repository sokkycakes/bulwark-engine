/* Bulwark engine - server-side builtins (world, precaches, info keys).
 * Networking builtins are accepted and dropped until the netcode exists.
 * ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/cvar.h"
#include "../common/cmd.h"
#include "../common/skel.h"
#include "../common/studio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define F vm->fofs
#define G vm->gofs

static float *ev(qcvm_t *vm, int e, int ofs) { return ofs < 0 ? NULL : &QC_EdictFields(vm, e)[ofs].f; }

static void bi_spawn(qcvm_t *vm) { QC_ReturnInt(vm, SV_Spawn()); }

static void bi_remove(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0);
	if (e <= 0) {
		Con_Printf("^3%s: tried to remove world\n", vm->name);
		return;
	}
	SV_UnlinkEdict(e);
	QC_FreeEdict(vm, e, (float)sv.time);
}

void W_bi_setorigin(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0);
	float *org = ev(vm, e, F.origin);
	if (!org)
		return;
	VectorCopy(QC_PVEC(vm, 1), org);
	SV_LinkEdict(e, false);
}

static void set_size(qcvm_t *vm, int e, const float *mins, const float *maxs)
{
	float *m = ev(vm, e, F.mins), *M = ev(vm, e, F.maxs), *s = ev(vm, e, F.size);
	for (int i = 0; i < 3; i++) {
		if (m)
			m[i] = mins[i];
		if (M)
			M[i] = maxs[i];
		if (s)
			s[i] = maxs[i] - mins[i];
	}
}

void W_bi_setsize(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0);
	set_size(vm, e, QC_PVEC(vm, 1), QC_PVEC(vm, 2));
	SV_LinkEdict(e, false);
}

static void bi_setmodel(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0);
	const char *m = QC_PSTR(vm, 1);
	int idx = SV_ModelIndex(m, false);
	if (*m && !idx) {
		Con_DPrintf("setmodel: %s was not precached\n", m);
		idx = SV_ModelIndex(m, true);
	}
	if (F.model >= 0)
		QC_EdictFields(vm, e)[F.model].i = *m ? QC_NewString(vm, m) : 0;
	QC_SetEFloat(vm, e, F.modelindex, (float)idx);
	if (idx && sv.modelinfo[idx].bsp)
		set_size(vm, e, sv.modelinfo[idx].mins, sv.modelinfo[idx].maxs);
	else {
		float zero[3] = {0, 0, 0};
		set_size(vm, e, zero, zero);
	}
	SV_LinkEdict(e, false);
}

static void bi_setmodelindex(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0), idx = (int)QC_PFLOAT(vm, 1);
	if (idx < 0 || idx >= sv.nummodels)
		idx = 0;
	if (F.model >= 0)
		QC_EdictFields(vm, e)[F.model].i = idx ? QC_NewString(vm, sv.models[idx]) : 0;
	QC_SetEFloat(vm, e, F.modelindex, (float)idx);
	SV_LinkEdict(e, false);
}

static void bi_precache_model(qcvm_t *vm)
{
	SV_ModelIndex(QC_PSTR(vm, 0), true);
	vm->globals[OFS_RETURN] = *QC_Parm(vm, 0);
}

static void bi_precache_sound(qcvm_t *vm)
{
	SV_SoundIndex(QC_PSTR(vm, 0), true);
	vm->globals[OFS_RETURN] = *QC_Parm(vm, 0);
}

static void bi_precache_file(qcvm_t *vm) { vm->globals[OFS_RETURN] = *QC_Parm(vm, 0); }

static void bi_getmodelindex(qcvm_t *vm)
{
	bool queryonly = vm->argc >= 2 && QC_PFLOAT(vm, 1);
	QC_ReturnFloat(vm, (float)SV_ModelIndex(QC_PSTR(vm, 0), !queryonly));
}

static void bi_modelnameforindex(qcvm_t *vm)
{
	int i = (int)QC_PFLOAT(vm, 0);
	if (i > 0 && i < sv.nummodels)
		QC_ReturnString(vm, sv.models[i]);
	else
		QC_ReturnInt(vm, 0);
}

static void bi_getsoundindex(qcvm_t *vm) { QC_ReturnFloat(vm, (float)SV_SoundIndex(QC_PSTR(vm, 0), false)); }

static void bi_soundnameforindex(qcvm_t *vm)
{
	int i = (int)QC_PFLOAT(vm, 0);
	if (i > 0 && i < sv.numsounds)
		QC_ReturnString(vm, sv.sounds[i]);
	else
		QC_ReturnInt(vm, 0);
}

static void do_trace(qcvm_t *vm, const float *start, const float *mins, const float *maxs, const float *end, int type, int ent)
{
	svtrace_t tr;
	SV_Move(start, mins, maxs, end, type, ent, &tr);
	SV_SetTraceGlobals(&tr);
}

void W_bi_traceline(qcvm_t *vm)
{
	float zero[3] = {0, 0, 0};
	do_trace(vm, QC_PVEC(vm, 0), zero, zero, QC_PVEC(vm, 1), (int)QC_PFLOAT(vm, 2), QC_PENT(vm, 3));
}

void W_bi_tracebox(qcvm_t *vm)
{
	do_trace(vm, QC_PVEC(vm, 0), QC_PVEC(vm, 1), QC_PVEC(vm, 2), QC_PVEC(vm, 3), (int)QC_PFLOAT(vm, 4), QC_PENT(vm, 5));
}

/* Sky is solid to movement, but pointcontents reports it as sky, as Quake
 * does, so the game can tell open sky from a wall. */
void W_bi_pointcontents(qcvm_t *vm)
{
	int bits = SV_PointContentsBits(QC_PVEC(vm, 0));
	QC_ReturnFloat(vm, (float)((bits & CONTENTS_SKY) ? CONTENT_SKY : SV_ContentsToQuake(bits)));
}

void W_bi_droptofloor(qcvm_t *vm)
{
	QC_ReturnFloat(vm, SV_DropToFloor(QC_GInt(vm, G.self)) ? 1 : 0);
}

void W_bi_checkbottom(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0);
	float *org = ev(vm, e, F.origin), end[3], zero[3] = {0, 0, 0};
	svtrace_t tr;
	VectorCopy(org, end);
	end[2] += QC_EFloat(vm, e, F.mins + 2) - 18;
	SV_Move(org, zero, zero, end, MOVE_NOMONSTERS, e, &tr);
	QC_ReturnFloat(vm, tr.fraction < 1 ? 1 : 0);
}

void W_bi_walkmove(qcvm_t *vm)
{
	int e = QC_GInt(vm, G.self);
	float yaw = QC_PFLOAT(vm, 0) * (float)(M_PI / 180), dist = QC_PFLOAT(vm, 1);
	float *org = ev(vm, e, F.origin), end[3];
	svtrace_t tr;
	if (!org) {
		QC_ReturnFloat(vm, 0);
		return;
	}
	end[0] = org[0] + cosf(yaw) * dist;
	end[1] = org[1] + sinf(yaw) * dist;
	end[2] = org[2];
	SV_Move(org, ev(vm, e, F.mins), ev(vm, e, F.maxs), end, MOVE_NORMAL, e, &tr);
	if (tr.fraction < 1 || tr.allsolid) {
		QC_ReturnFloat(vm, 0);
		return;
	}
	VectorCopy(tr.endpos, org);
	SV_LinkEdict(e, true);
	QC_ReturnFloat(vm, 1);
}

void W_bi_changeyaw(qcvm_t *vm)
{
	int e = QC_GInt(vm, G.self);
	int ideal = QC_FieldOfs(vm, "ideal_yaw"), speed = QC_FieldOfs(vm, "yaw_speed");
	float *ang = ev(vm, e, F.angles), cur, target, move;
	if (!ang || ideal < 0)
		return;
	cur = fmodf(ang[1], 360);
	target = QC_EFloat(vm, e, ideal);
	move = target - cur;
	if (move > 180)
		move -= 360;
	else if (move < -180)
		move += 360;
	if (speed >= 0) {
		float s = QC_EFloat(vm, e, speed);
		if (move > s)
			move = s;
		else if (move < -s)
			move = -s;
	}
	ang[1] = fmodf(cur + move, 360);
}

void W_bi_touchtriggers(qcvm_t *vm)
{
	int e = vm->argc >= 1 ? QC_PENT(vm, 0) : QC_GInt(vm, G.self);
	if (vm->argc >= 2) {
		float *org = ev(vm, e, F.origin);
		if (org)
			VectorCopy(QC_PVEC(vm, 1), org);
	}
	SV_LinkEdict(e, true);
}

/* checkpvs(viewpos, entity): whether any of the entity's box may be seen
 * from the point; its centre and corners are tested */
void W_bi_checkpvs(qcvm_t *vm)
{
	const bspmap_t *map = SV_CurrentWorld();
	const float *view = QC_PVEC(vm, 0);
	int e = QC_PENT(vm, 1);
	float *org = ev(vm, e, F.origin), *mins = ev(vm, e, F.mins), *maxs = ev(vm, e, F.maxs);
	if (!map || !map->vis || !org) {
		QC_ReturnFloat(vm, 1);
		return;
	}
	for (int i = -1; i < 8; i++) {
		float p[3];
		for (int k = 0; k < 3; k++) {
			float lo = mins ? mins[k] : 0, hi = maxs ? maxs[k] : 0;
			p[k] = org[k] + (i < 0 ? (lo + hi) * 0.5f : ((i >> k) & 1) ? hi : lo);
		}
		if (BSP_CheckPVS(map, view, p)) {
			QC_ReturnFloat(vm, 1);
			return;
		}
	}
	QC_ReturnFloat(vm, 0);
}
static void bi_aim(qcvm_t *vm)
{
	float v[3];
	for (int i = 0; i < 3; i++)
		v[i] = QC_GFloat(vm, G.v_forward + i);
	QC_ReturnVector(vm, v);
}

static void bi_lightstyle(qcvm_t *vm)
{
	int s = (int)QC_PFLOAT(vm, 0);
	if (s < 0 || s >= SV_MAX_LIGHTSTYLES)
		return;
	Mem_Free(sv.lightstyles[s]);
	sv.lightstyles[s] = Mem_Strdup(QC_PSTR(vm, 1));
	if (!sv.spawning)
		SV_BroadcastLightstyle(s, sv.lightstyles[s]);
}

static void bi_getlightstyle(qcvm_t *vm)
{
	int s = (int)QC_PFLOAT(vm, 0);
	QC_ReturnString(vm, (s >= 0 && s < SV_MAX_LIGHTSTYLES && sv.lightstyles[s]) ? sv.lightstyles[s] : "");
}

static void bi_getlight(qcvm_t *vm)
{
	float v[3] = {0, 0, 0};
	QC_ReturnVector(vm, v); /* TODO: sample the lightgrid */
}

static void bi_particleeffectnum(qcvm_t *vm)
{
	const char *n = QC_PSTR(vm, 0);
	if (!*n) {
		QC_ReturnFloat(vm, 0);
		return;
	}
	for (int i = 0; i < sv.numparticles; i++)
		if (!strcmp(sv.particles[i], n)) {
			QC_ReturnFloat(vm, (float)(i + 1));
			return;
		}
	if (sv.numparticles >= SV_MAX_PARTICLES) {
		QC_ReturnFloat(vm, 0);
		return;
	}
	sv.particles[sv.numparticles++] = Mem_Strdup(n);
	if (!sv.spawning)
		SV_BroadcastPrecache(PRECACHE_PARTICLE, sv.numparticles, n);
	QC_ReturnFloat(vm, (float)sv.numparticles);
}


static void bi_dropped(qcvm_t *vm) { (void)vm; }

/* logfrag(killer, victim): a line in the frag log (fraglogfile) */
static void bi_logfrag(qcvm_t *vm) { SV_LogFrag(QC_PENT(vm, 0), QC_PENT(vm, 1)); }

static void bi_localsound(qcvm_t *vm) { (void)vm; }





static void bi_serverkey(qcvm_t *vm)
{
	char buf[1024];
	SV_InfoGet(sv.serverinfo, QC_PSTR(vm, 0), buf, sizeof(buf));
	if (buf[0])
		QC_ReturnString(vm, buf);
	else
		QC_ReturnInt(vm, 0);
}

static void bi_serverkeyfloat(qcvm_t *vm)
{
	char buf[1024];
	SV_InfoGet(sv.serverinfo, QC_PSTR(vm, 0), buf, sizeof(buf));
	QC_ReturnFloat(vm, buf[0] ? (float)atof(buf) : (vm->argc >= 2 ? QC_PFLOAT(vm, 1) : 0));
}

static void bi_setserverkey(qcvm_t *vm)
{
	SV_InfoSet(sv.serverinfo, sizeof(sv.serverinfo), QC_PSTR(vm, 0), QC_PSTR(vm, 1));
	SV_BroadcastServerinfo(QC_PSTR(vm, 0), QC_PSTR(vm, 1));
}

static void bi_setlocalinfo(qcvm_t *vm) { SV_InfoSet(sv.localinfo, sizeof(sv.localinfo), QC_PSTR(vm, 0), QC_PSTR(vm, 1)); }

static void bi_getlocalinfo(qcvm_t *vm)
{
	char buf[1024];
	SV_InfoGet(sv.localinfo, QC_PSTR(vm, 0), buf, sizeof(buf));
	QC_ReturnString(vm, buf);
}

static void bi_changelevel(qcvm_t *vm)
{
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "changelevel \"%s\"\n", QC_PSTR(vm, 0));
	Cbuf_AddText(cmd);
}

static void bi_makestatic(qcvm_t *vm)
{
	/* static entities only matter to clients; drop the server copy */
	int e = QC_PENT(vm, 0);
	if (e > 0)
		QC_FreeEdict(vm, e, (float)sv.time);
}


static void bi_registercommand(qcvm_t *vm)
{
	const char *n = QC_PSTR(vm, 0);
	for (int i = 0; i < sv.numcommands; i++)
		if (!Str_ICmp(sv.commands[i], n))
			return;
	if (sv.numcommands < SV_MAX_COMMANDS)
		sv.commands[sv.numcommands++] = Mem_Strdup(n);
}

static void bi_getentitytoken(qcvm_t *vm)
{
	char tok[4096];
	if (vm->argc >= 1)
		sv.entitytoken_pos = QC_PSTR(vm, 0); /* not persistent; only valid for this call chain */
	else if (!sv.entitytoken_pos && sv.world)
		sv.entitytoken_pos = sv.world->entities;
	sv.entitytoken_pos = COM_Parse(sv.entitytoken_pos, tok, sizeof(tok));
	if (!sv.entitytoken_pos) {
		QC_ReturnInt(vm, 0);
		return;
	}
	QC_ReturnString(vm, tok);
}

/* frame queries: skeletal and studio models only so far (alias models
 * need their frame names read on the server too) */
static skelmodel_t *skel_arg(qcvm_t *vm)
{
	int i = (int)QC_PFLOAT(vm, 0);
	if (i <= 0 || i >= sv.nummodels || Studio_Cached(sv.models[i]))
		return NULL; /* studio models answer from their sequences (studio_arg) */
	return Skel_LoadModel(sv.models[i]);
}

/* alias models (.mdl, .md2) have no skeleton; their frame groups come from
 * a "<model>.framegroups" file, as on the client */
static framegroup_t *alias_groups(qcvm_t *vm, int *count)
{
	static struct {
		char name[128];
		framegroup_t *groups;
		int count;
	} cache[256];
	static int numcached;
	int i = (int)QC_PFLOAT(vm, 0);
	const char *name;
	*count = 0;
	if (i <= 0 || i >= sv.nummodels || !sv.models[i] || Studio_Cached(sv.models[i]))
		return NULL;
	name = sv.models[i];
	for (int c = 0; c < numcached; c++)
		if (!Str_ICmp(cache[c].name, name)) {
			*count = cache[c].count;
			return cache[c].groups;
		}
	if (numcached == (int)BW_ARRAY_COUNT(cache))
		return NULL;
	Str_Copy(cache[numcached].name, name, sizeof(cache[numcached].name));
	cache[numcached].groups = Skel_LoadFramegroups(name, &cache[numcached].count);
	*count = cache[numcached].count;
	return cache[numcached++].groups;
}

static framegroup_t *group_arg(qcvm_t *vm, int n)
{
	skelmodel_t *m = skel_arg(vm);
	int f = (int)QC_PFLOAT(vm, n), count;
	framegroup_t *g;
	if (m)
		return f >= 0 && f < m->numanims ? &m->anims[f] : NULL;
	g = alias_groups(vm, &count);
	return g && f >= 0 && f < count ? &g[f] : NULL;
}

/* the frame queries take IQM framegroups, or GoldSrc studio sequences */
static const studio_model_t *studio_arg(qcvm_t *vm)
{
	return QC_StudioModel(vm, (int)QC_PFLOAT(vm, 0));
}

static void bi_frameforname(qcvm_t *vm)
{
	skelmodel_t *m = skel_arg(vm);
	const studio_model_t *sm = m ? NULL : studio_arg(vm);
	const char *name = QC_PSTR(vm, 1);
	for (int i = 0; m && i < m->numanims; i++)
		if (!Str_ICmp(m->anims[i].name, name)) {
			QC_ReturnFloat(vm, (float)i);
			return;
		}
	for (int i = 0; sm && i < sm->numseqs; i++)
		if (!Str_ICmp(sm->seqs[i].label, name)) {
			QC_ReturnFloat(vm, (float)i);
			return;
		}
	QC_ReturnFloat(vm, -1);
}

static void bi_frameduration(qcvm_t *vm)
{
	framegroup_t *g = group_arg(vm, 1);
	if (g)
		QC_ReturnFloat(vm, g->count / g->fps);
	else
		QC_ReturnFloat(vm, Studio_SeqDuration(studio_arg(vm), (int)QC_PFLOAT(vm, 1)));
}

static void bi_frametoname(qcvm_t *vm)
{
	framegroup_t *g = group_arg(vm, 1);
	const studio_model_t *sm = g ? NULL : studio_arg(vm);
	int f = (int)QC_PFLOAT(vm, 1);
	if (g)
		QC_ReturnString(vm, g->name);
	else
		QC_ReturnString(vm, sm && f >= 0 && f < sm->numseqs ? sm->seqs[f].label : "");
}

static void bi_modelframecount(qcvm_t *vm)
{
	skelmodel_t *m = skel_arg(vm);
	const studio_model_t *sm = m ? NULL : studio_arg(vm);
	QC_ReturnFloat(vm, m ? (float)m->numanims : sm ? (float)sm->numseqs : 0);
}


const char *SV_ModelName(qcvm_t *vm, int index)
{
	return index > 0 && index < sv.nummodels ? sv.models[index] : NULL;
}



/* no rigid-body physics yet; Nuclide falls back to its own movement */
static void bi_physics_supported(qcvm_t *vm) { QC_ReturnFloat(vm, 0); }
static void bi_nop(qcvm_t *vm) { (void)vm; }

const qc_builtin_t sv_builtins[] = {
	{"physics_supported", bi_physics_supported},
	/* MOVETYPE_PHYSICS is not simulated, so these have nothing to act on */
	{"physics_enable", bi_nop}, {"physics_addforce", bi_nop}, {"physics_addtorque", bi_nop},
	/* the server game's debug polygons (bone and path overlays) draw nowhere */
	{"R_BeginPolygon", bi_nop}, {"R_PolygonVertex", bi_nop}, {"R_EndPolygon", bi_nop},
	{"R_EndPolygonRibbon", bi_nop},
	{"getsurfacenearpoint", W_bi_getsurfacenearpoint}, {"getsurfacetexture", W_bi_getsurfacetexture},
	{"spawn", bi_spawn}, {"remove", bi_remove}, {"removeinstant", bi_remove},
	{"setorigin", W_bi_setorigin}, {"setsize", W_bi_setsize}, {"setmodel", bi_setmodel},
	{"setmodelindex", bi_setmodelindex}, {"precache_model", bi_precache_model},
	{"precache_model2", bi_precache_model}, {"precache_vwep_model", bi_precache_model},
	{"precache_sound", bi_precache_sound}, {"precache_sound2", bi_precache_sound},
	{"precache_file", bi_precache_file}, {"precache_file2", bi_precache_file},
	{"getmodelindex", bi_getmodelindex}, {"modelnameforindex", bi_modelnameforindex},
	{"getsoundindex", bi_getsoundindex}, {"soundnameforindex", bi_soundnameforindex},
	{"traceline", W_bi_traceline}, {"tracebox", W_bi_tracebox}, {"pointcontents", W_bi_pointcontents},
	{"droptofloor", W_bi_droptofloor}, {"checkbottom", W_bi_checkbottom}, {"walkmove", W_bi_walkmove},
	{"changeyaw", W_bi_changeyaw}, {"touchtriggers", W_bi_touchtriggers}, {"checkpvs", W_bi_checkpvs},
	{"aim", bi_aim}, {"lightstyle", bi_lightstyle}, {"lightstylestatic", bi_lightstyle},
	{"getlightstyle", bi_getlightstyle}, {"getlight", bi_getlight},
	{"particleeffectnum", bi_particleeffectnum}, {"particle", bi_dropped}, {"effect", bi_dropped},
	{"ambientsound", bi_dropped},
	{"localsound", bi_localsound}, {"serverkey", bi_serverkey}, {"serverkeyfloat", bi_serverkeyfloat},
	{"setserverkey", bi_setserverkey}, {"setlocalinfo", bi_setlocalinfo},
	{"getlocalinfo", bi_getlocalinfo}, {"changelevel", bi_changelevel},
	{"makestatic", bi_makestatic}, {"registercommand", bi_registercommand}, {"getentitytoken", bi_getentitytoken},
	{"frameforname", bi_frameforname}, {"frameduration", bi_frameduration}, {"frametoname", bi_frametoname},
	{"modelframecount", bi_modelframecount},
	{"setattachment", bi_dropped}, {"logfrag", bi_logfrag},
	{"openportal", bi_dropped},
	{NULL, NULL}};
