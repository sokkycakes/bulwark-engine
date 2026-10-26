/* Bulwark engine - server: progs lifecycle, map loading, entity spawning,
 * think and movement physics. ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/cvar.h"
#include "../common/cmd.h"
#include "../common/fs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define F sv.vm->fofs
#define G sv.vm->gofs

server_t sv;

static cvar_t *sv_gravity, *sv_maxvelocity, *sv_progs, *pr_maxedicts, *pr_heapsize, *sv_tickrate;

/* --- info strings ---------------------------------------------------- */

const char *SV_InfoGet(const char *info, const char *key, char *out, size_t outsize)
{
	const char *p = info;
	size_t klen = strlen(key);
	out[0] = 0;
	while (*p == '\\') {
		const char *k = p + 1, *ke = strchr(k, '\\'), *v, *ve;
		if (!ke)
			break;
		v = ke + 1;
		ve = strchr(v, '\\');
		if (!ve)
			ve = v + strlen(v);
		if ((size_t)(ke - k) == klen && !strncmp(k, key, klen)) {
			size_t n = BW_MIN((size_t)(ve - v), outsize - 1);
			memcpy(out, v, n);
			out[n] = 0;
			return out;
		}
		p = ve;
	}
	return out;
}

void SV_InfoSet(char *info, size_t size, const char *key, const char *value)
{
	char rebuilt[8192];
	const char *p = info;
	size_t klen = strlen(key);
	if (strchr(key, '\\') || strchr(value, '\\'))
		return;
	rebuilt[0] = 0;
	while (*p == '\\') {
		const char *k = p + 1, *ke = strchr(k, '\\'), *v, *ve;
		if (!ke)
			break;
		v = ke + 1;
		ve = strchr(v, '\\');
		if (!ve)
			ve = v + strlen(v);
		if (!((size_t)(ke - k) == klen && !strncmp(k, key, klen))) {
			size_t l = strlen(rebuilt), n = (size_t)(ve - p);
			if (l + n < sizeof(rebuilt)) {
				memcpy(rebuilt + l, p, n);
				rebuilt[l + n] = 0;
			}
		}
		p = ve;
	}
	if (*value) {
		Str_Cat(rebuilt, "\\", sizeof(rebuilt));
		Str_Cat(rebuilt, key, sizeof(rebuilt));
		Str_Cat(rebuilt, "\\", sizeof(rebuilt));
		Str_Cat(rebuilt, value, sizeof(rebuilt));
	}
	Str_Copy(info, rebuilt, size);
}

/* --- precaches ------------------------------------------------------- */

static void model_bounds(int index)
{
	svmodel_t *mi = &sv.modelinfo[index];
	const char *name = sv.models[index];
	memset(mi, 0, sizeof(*mi));
	if (name[0] == '*' && sv.world) {
		int sub = atoi(name + 1);
		if (sub > 0 && sub < sv.world->nummodels) {
			mi->bsp = sv.world;
			mi->submodel = sub;
			VectorCopy(sv.world->models[sub].mins, mi->mins);
			VectorCopy(sv.world->models[sub].maxs, mi->maxs);
		}
	} else if (!strcmp(name, sv.worldpath) && sv.world) {
		mi->bsp = sv.world;
		VectorCopy(sv.world->models[0].mins, mi->mins);
		VectorCopy(sv.world->models[0].maxs, mi->maxs);
	} else {
		size_t n = strlen(name);
		if (n > 4 && !Str_ICmp(name + n - 4, ".bsp")) {
			/* stand-alone bsp models (item boxes and the like) */
			bspmap_t *b = BSP_Load(name);
			if (b && b->nummodels > 0) {
				mi->bsp = b;
				VectorCopy(b->models[0].mins, mi->mins);
				VectorCopy(b->models[0].maxs, mi->maxs);
			} else {
				BSP_Free(b);
			}
		}
		/* TODO: bounds for mesh models (IQM/VVM/MDL) */
	}
}

int SV_ModelIndex(const char *name, bool precache)
{
	if (!name || !*name)
		return 0;
	for (int i = 1; i < sv.nummodels; i++)
		if (!strcmp(sv.models[i], name))
			return i;
	if (!precache)
		return 0;
	if (sv.nummodels >= SV_MAX_MODELS) {
		Con_Printf("^3model precache list full, dropping %s\n", name);
		return 0;
	}
	sv.models[sv.nummodels] = Mem_Strdup(name);
	model_bounds(sv.nummodels);
	if (!sv.spawning)
		SV_BroadcastPrecache(PRECACHE_MODEL, sv.nummodels, name);
	return sv.nummodels++;
}

int SV_SoundIndex(const char *name, bool precache)
{
	if (!name || !*name)
		return 0;
	for (int i = 1; i < sv.numsounds; i++)
		if (!strcmp(sv.sounds[i], name))
			return i;
	if (!precache || sv.numsounds >= SV_MAX_SOUNDS)
		return 0;
	sv.sounds[sv.numsounds] = Mem_Strdup(name);
	if (!sv.spawning)
		SV_BroadcastPrecache(PRECACHE_SOUND, sv.numsounds, name);
	return sv.numsounds++;
}

/* --- entities -------------------------------------------------------- */

static void init_edict_defaults(int e)
{
	/* fields that mean "everything" when unset in this engine family */
	QC_SetEFloat(sv.vm, e, F.dimension_solid, 255);
	QC_SetEFloat(sv.vm, e, F.dimension_hit, 255);
}

int SV_Spawn(void)
{
	int e = QC_SpawnEdict(sv.vm);
	init_edict_defaults(e);
	return e;
}

/* Spawns every entity in the map's entity lump. For each entity the
 * fields named by its keys are filled in, __fullspawndata is set to the
 * entity's raw text, and the progs' CheckSpawn(spawnfunc) is called if it
 * has one; otherwise the function named by the classname is called. */
static void spawn_map_entities(const char *data)
{
	qcvm_t *vm = sv.vm;
	int checkspawn = QC_FindFunction(vm, "CheckSpawn");
	int fullspawn = QC_GlobalOfs(vm, "__fullspawndata");
	int count = 0, failed = 0;
	bool world = true;

	for (;;) {
		char token[64];
		const char *blockstart, *next, *end;
		bool ok;
		int e;
		const char *peek = COM_Parse(data, token, sizeof(token));
		if (!peek)
			break;
		if (strcmp(token, "{")) {
			Con_Printf("^3entity lump: expected {, got %s\n", token);
			break;
		}
		/* the block starts at its opening brace */
		blockstart = peek - 1;
		e = world ? 0 : SV_Spawn();
		if (world)
			init_edict_defaults(0);
		next = QC_ParseEdict(vm, e, data, &ok);
		if (!ok || !next) {
			Con_Printf("^3entity lump: parse error\n");
			break;
		}
		end = next;
		data = next;

		{
			const char *classname = QC_GetString(vm, QC_EInt(vm, e, F.classname));
			int fn = 0;
			if (*classname) {
				/* fteqcc names class constructors spawnfunc_<class> */
				char sfname[128];
				snprintf(sfname, sizeof(sfname), "spawnfunc_%s", classname);
				if (!(fn = QC_FindFunction(vm, sfname)))
					fn = QC_FindFunction(vm, classname);
			}
			if (!fn && e)
				failed++;
			if (fullspawn >= 0)
				vm->globals[fullspawn].i = QC_NewStringLen(vm, blockstart, (size_t)(end - blockstart));
			QC_SetGInt(vm, G.self, e);
			QC_SetGInt(vm, G.other, 0);
			if (checkspawn) {
				vm->globals[OFS_PARM(0)].i = fn;
				vm->argc = 1;
				QC_Execute(vm, checkspawn);
			} else if (fn) {
				QC_Execute(vm, fn);
			} else {
				if (*classname)
					Con_DPrintf("no spawn function for %s\n", classname);
				if (e)
					QC_FreeEdict(vm, e, (float)sv.time);
			}
			if (fullspawn >= 0)
				vm->globals[fullspawn].i = 0;
		}
		count++;
		world = false;
	}
	Con_Printf("%d map entities parsed, %d with no QC function for their classname\n", count, failed);
}

/* --- lifecycle ------------------------------------------------------- */

static const qc_builtin_t *const sv_tables[] = {sv_client_builtins, sv_builtins, sv_voxel_builtins, qc_builtins_core, qc_builtins_extra, NULL};

void SV_Shutdown(void)
{
	SV_ClearRoutes();
	if (sv.vm) {
		if (QC_FindFunction(sv.vm, "SV_Shutdown"))
			QC_ExecuteByName(sv.vm, "SV_Shutdown");
		QC_Free(sv.vm);
	}
	for (int i = 1; i < sv.nummodels; i++) {
		if (sv.modelinfo[i].bsp && sv.modelinfo[i].bsp != sv.world)
			BSP_Free(sv.modelinfo[i].bsp);
		Mem_Free(sv.models[i]);
	}
	for (int i = 1; i < sv.numsounds; i++)
		Mem_Free(sv.sounds[i]);
	for (int i = 0; i < sv.numparticles; i++)
		Mem_Free(sv.particles[i]);
	for (int i = 0; i < SV_MAX_LIGHTSTYLES; i++)
		Mem_Free(sv.lightstyles[i]);
	for (int i = 0; i < sv.numcommands; i++)
		Mem_Free(sv.commands[i]);
	BSP_Free(sv.world);
	SV_VoxelShutdown();
	memset(&sv, 0, sizeof(sv));
}

bool SV_SpawnServer(const char *mapname)
{
	char path[160];
	size_t len;
	char *progs;
	qcvm_t *vm;
	char keep_serverinfo[8192], keep_localinfo[8192];

	Str_Copy(keep_serverinfo, sv.serverinfo, sizeof(keep_serverinfo));
	Str_Copy(keep_localinfo, sv.localinfo, sizeof(keep_localinfo));
	SV_Shutdown();
	Str_Copy(sv.serverinfo, keep_serverinfo, sizeof(sv.serverinfo));
	Str_Copy(sv.localinfo, keep_localinfo, sizeof(sv.localinfo));

	snprintf(path, sizeof(path), "maps/%s.bsp", mapname);
	Host_Loading("server_world", mapname, 0.0f);
	if (!FS_FileExists(path)) {
		/* a void map: entities only, the level is built from voxels */
		char vpath[160];
		snprintf(vpath, sizeof(vpath), "maps/%s.vxmap", mapname);
		if (FS_FileExists(vpath))
			Str_Copy(path, vpath, sizeof(path));
	}
	sv.world = BSP_Load(path);
	if (!sv.world) {
		Con_Printf("couldn't load map %s\n", path);
		Host_Loading(NULL, NULL, 0);
		return false;
	}
	Str_Copy(sv.mapname, mapname, sizeof(sv.mapname));
	Str_Copy(sv.worldpath, path, sizeof(sv.worldpath));
	Cvar_SetDirect(Cvar_Find("mapname"), mapname);
	SV_InfoSet(sv.serverinfo, sizeof(sv.serverinfo), "map", mapname);
	SV_SyncServerinfoCvars(false);

	Host_Loading("server_progs", mapname, 0.10f);
	progs = FS_LoadFile(sv_progs->string, &len);
	if (!progs) {
		Con_Printf("couldn't load %s\n", sv_progs->string);
		Host_Loading(NULL, NULL, 0);
		return false;
	}
	vm = QC_Load(sv_progs->string, progs, len, QCVM_SERVER, sv_tables, BW_MAX(64, pr_maxedicts->ivalue),
	             (size_t)BW_MAX(1, pr_heapsize->ivalue) * 1024 * 1024);
	Mem_Free(progs);
	if (!vm) {
		Host_Loading(NULL, NULL, 0);
		return false;
	}
	vm->host = &sv;
	vm->modelname = SV_ModelName;
	sv.vm = vm;
	SV_SetupClientSlots();
	vm->reserved_edicts = svs.maxclients;
	vm->num_edicts = svs.maxclients + 1;
	for (int i = 1; i <= svs.maxclients; i++)
		QC_ClearEdict(vm, i);
	sv.nummodels = 1;
	sv.numsounds = 1;
	sv.time = 1.0; /* start a second in so think times of 0 mean "never" */
	sv.frametime = 1.0f / BW_MAX(1.0f, sv_tickrate->value);
	sv.active = true;
	sv.spawning = true;

	SV_ModelIndex(sv.worldpath, true);
	for (int i = 1; i < sv.world->nummodels; i++) {
		char sub[16];
		snprintf(sub, sizeof(sub), "*%d", i);
		SV_ModelIndex(sub, true);
	}

	QC_SetGFloat(vm, G.time, (float)sv.time);
	QC_SetGFloat(vm, G.frametime, sv.frametime);
	if (G.mapname >= 0)
		vm->globals[G.mapname].i = QC_NewString(vm, mapname);
	QC_SetGInt(vm, G.world, 0);

	/* world entity basics before any QC runs */
	QC_ClearEdict(vm, 0);
	init_edict_defaults(0);
	if (F.model >= 0)
		QC_EdictFields(vm, 0)[F.model].i = QC_NewString(vm, sv.worldpath);
	QC_SetEFloat(vm, 0, F.modelindex, 1);
	QC_SetEFloat(vm, 0, F.solid, SOLID_BSP);
	QC_SetEFloat(vm, 0, F.movetype, MOVETYPE_PUSH);
	if (F.mins >= 0) {
		memcpy(&QC_EdictFields(vm, 0)[F.mins], sv.world->models[0].mins, sizeof(float) * 3);
		memcpy(&QC_EdictFields(vm, 0)[F.maxs], sv.world->models[0].maxs, sizeof(float) * 3);
	}

	/* init(prevprogs) runs before entities exist; initents() just before worldspawn */
	{
		int f = QC_FindFunction(vm, "init");
		if (f) {
			vm->globals[OFS_PARM(0)].f = 0;
			vm->argc = 1;
			QC_Execute(vm, f);
		}
	}
	if (QC_FindFunction(vm, "main"))
		QC_ExecuteByName(vm, "main");
	if (QC_FindFunction(vm, "initents")) {
		QC_SetGInt(vm, G.self, 0);
		QC_ExecuteByName(vm, "initents");
	}

	Host_Loading("server_entities", mapname, 0.15f);
	spawn_map_entities(sv.world->entities);
	sv.spawning = false;
	SV_ClientsMapChanged();
	QC_CollectGarbage(vm, true);
	Con_Printf("map %s: %d entities in use, %d models, %d sounds precached\n", mapname,
	           vm->num_edicts, sv.nummodels - 1, sv.numsounds - 1);
	return true;
}

/* --- physics --------------------------------------------------------- */

static float *ev(int e, int ofs) { return ofs < 0 ? NULL : &QC_EdictFields(sv.vm, e)[ofs].f; }

static bool run_think(int e, float thinktime_limit)
{
	qcvm_t *vm = sv.vm;
	float thinktime = QC_EFloat(vm, e, F.nextthink);
	func_t think;
	if (thinktime <= 0 || thinktime > thinktime_limit)
		return true;
	if (thinktime < sv.time)
		thinktime = (float)sv.time;
	QC_SetEFloat(vm, e, F.nextthink, 0);
	think = QC_EInt(vm, e, F.think);
	if (!think)
		return true;
	QC_SetGFloat(vm, G.time, thinktime);
	QC_SetGInt(vm, G.self, e);
	QC_SetGInt(vm, G.other, 0);
	QC_Execute(vm, think);
	return !vm->edicts[e].free;
}

static void clamp_velocity(float *vel)
{
	float max = sv_maxvelocity->value;
	for (int i = 0; i < 3; i++) {
		if (vel[i] > max)
			vel[i] = max;
		else if (vel[i] < -max)
			vel[i] = -max;
	}
}

static void impact(int e, int other)
{
	qcvm_t *vm = sv.vm;
	int oldself = QC_GInt(vm, G.self), oldother = QC_GInt(vm, G.other);
	func_t t = QC_EInt(vm, e, F.touch);
	if (t && QC_EFloat(vm, e, F.solid) != SOLID_NOT) {
		QC_SetGInt(vm, G.self, e);
		QC_SetGInt(vm, G.other, other);
		QC_Execute(vm, t);
	}
	if (other > 0 && !vm->edicts[other].free) {
		t = QC_EInt(vm, other, F.touch);
		if (t && QC_EFloat(vm, other, F.solid) != SOLID_NOT) {
			QC_SetGInt(vm, G.self, other);
			QC_SetGInt(vm, G.other, e);
			QC_Execute(vm, t);
		}
	}
	QC_SetGInt(vm, G.self, oldself);
	QC_SetGInt(vm, G.other, oldother);
}

static void clip_velocity(const float *in, const float *normal, float *out, float overbounce)
{
	float backoff = DotProduct(in, normal) * overbounce;
	for (int i = 0; i < 3; i++) {
		out[i] = in[i] - normal[i] * backoff;
		if (out[i] > -0.1f && out[i] < 0.1f)
			out[i] = 0;
	}
}

/* toss, bounce and fly-missile movement */
static void physics_toss(int e, int movetype)
{
	float *org = ev(e, F.origin), *vel = ev(e, F.velocity), *ang = ev(e, F.angles), *avel = ev(e, F.avelocity);
	float end[3], dt = sv.frametime;
	int flags = (int)QC_EFloat(sv.vm, e, F.flags);
	svtrace_t tr;
	if (!org || !vel)
		return;
	if ((flags & FL_ONGROUND) && movetype != MOVETYPE_FLY && movetype != MOVETYPE_FLYMISSILE) {
		if (vel[0] == 0 && vel[1] == 0 && vel[2] == 0)
			return;
		QC_SetEFloat(sv.vm, e, F.flags, (float)(flags & ~FL_ONGROUND));
	}
	clamp_velocity(vel);
	if (movetype == MOVETYPE_TOSS || movetype == MOVETYPE_BOUNCE || movetype == MOVETYPE_PHYSICS) {
		float grav = F.gravity >= 0 && QC_EFloat(sv.vm, e, F.gravity) ? QC_EFloat(sv.vm, e, F.gravity) : 1;
		vel[2] -= grav * sv_gravity->value * dt;
	}
	if (ang && avel)
		for (int i = 0; i < 3; i++)
			ang[i] += avel[i] * dt;
	for (int i = 0; i < 3; i++)
		end[i] = org[i] + vel[i] * dt;
	SV_Move(org, ev(e, F.mins), ev(e, F.maxs), end, movetype == MOVETYPE_FLYMISSILE ? MOVE_MISSILE : MOVE_NORMAL, e, &tr);
	VectorCopy(tr.endpos, org);
	SV_LinkEdict(e, true);
	if (sv.vm->edicts[e].free || tr.fraction == 1)
		return;
	if (tr.allsolid) {
		/* stuck: stop rather than tunnel */
		VectorClear(vel);
		return;
	}
	clip_velocity(vel, tr.normal, vel, (movetype == MOVETYPE_BOUNCE) ? 1.5f : 1.0f);
	if (tr.normal[2] > 0.7f && movetype != MOVETYPE_FLY && movetype != MOVETYPE_FLYMISSILE) {
		if (vel[2] < 60 || movetype != MOVETYPE_BOUNCE) {
			QC_SetEFloat(sv.vm, e, F.flags, (float)((int)QC_EFloat(sv.vm, e, F.flags) | FL_ONGROUND));
			QC_SetEInt(sv.vm, e, F.groundentity, tr.ent);
			VectorClear(vel);
			if (avel)
				VectorClear(avel);
		}
	}
	impact(e, tr.ent);
}

/* brush movers: move without collision for now and advance ltime */
static void physics_pusher(int e)
{
	float *org = ev(e, F.origin), *vel = ev(e, F.velocity), *ang = ev(e, F.angles), *avel = ev(e, F.avelocity);
	float ltime = QC_EFloat(sv.vm, e, F.ltime), oldltime = ltime;
	float thinktime = QC_EFloat(sv.vm, e, F.nextthink);
	float movetime = sv.frametime;
	if (thinktime < ltime + sv.frametime) {
		movetime = thinktime - ltime;
		if (movetime < 0)
			movetime = 0;
	}
	if (movetime > 0 && org && vel) {
		/* TODO: push and block entities that are in the way */
		for (int i = 0; i < 3; i++) {
			org[i] += vel[i] * movetime;
			if (ang && avel)
				ang[i] += avel[i] * movetime;
		}
		QC_SetEFloat(sv.vm, e, F.ltime, ltime + movetime);
		SV_LinkEdict(e, false);
	}
	thinktime = QC_EFloat(sv.vm, e, F.nextthink);
	if (thinktime > oldltime && thinktime <= QC_EFloat(sv.vm, e, F.ltime)) {
		func_t think = QC_EInt(sv.vm, e, F.think);
		QC_SetEFloat(sv.vm, e, F.nextthink, 0);
		QC_SetGFloat(sv.vm, G.time, (float)sv.time);
		QC_SetGInt(sv.vm, G.self, e);
		QC_SetGInt(sv.vm, G.other, 0);
		if (think)
			QC_Execute(sv.vm, think);
	}
}

static void physics_noclip(int e)
{
	float *org = ev(e, F.origin), *vel = ev(e, F.velocity), *ang = ev(e, F.angles), *avel = ev(e, F.avelocity);
	if (!org || !vel)
		return;
	for (int i = 0; i < 3; i++) {
		org[i] += vel[i] * sv.frametime;
		if (ang && avel)
			ang[i] += avel[i] * sv.frametime;
	}
	SV_LinkEdict(e, false);
}

static void run_entity(int e)
{
	qcvm_t *vm = sv.vm;
	int movetype = (int)QC_EFloat(vm, e, F.movetype);
	func_t custom = QC_EInt(vm, e, F.customphysics);
	float limit = (float)(sv.time + sv.frametime * 0.5);

	if (custom) {
		/* the progs replaces engine movement for this entity */
		QC_SetGInt(vm, G.self, e);
		QC_SetGInt(vm, G.other, 0);
		QC_SetGFloat(vm, G.time, (float)sv.time);
		QC_Execute(vm, custom);
		if (!vm->edicts[e].free)
			run_think(e, limit);
		return;
	}
	switch (movetype) {
	case MOVETYPE_PUSH:
		physics_pusher(e);
		return;
	case MOVETYPE_NOCLIP:
		if (run_think(e, limit))
			physics_noclip(e);
		return;
	case MOVETYPE_TOSS:
	case MOVETYPE_BOUNCE:
	case MOVETYPE_BOUNCEMISSILE:
	case MOVETYPE_FLY:
	case MOVETYPE_FLYMISSILE:
	case MOVETYPE_PHYSICS:
		if (run_think(e, limit))
			physics_toss(e, movetype == MOVETYPE_BOUNCEMISSILE ? MOVETYPE_BOUNCE : movetype);
		return;
	case MOVETYPE_STEP:
		/* monsters fall when not on ground; walking is done by the progs */
		if (!((int)QC_EFloat(vm, e, F.flags) & FL_ONGROUND))
			physics_toss(e, MOVETYPE_TOSS);
		run_think(e, limit);
		return;
	case MOVETYPE_WALK: /* player movement is run by the progs' own pmove */
	default:
		run_think(e, limit);
		return;
	}
}

void SV_Physics(void)
{
	qcvm_t *vm = sv.vm;
	QC_SetGFloat(vm, G.time, (float)sv.time);
	QC_SetGFloat(vm, G.frametime, sv.frametime);
	SV_RunRoutes();
	if (QC_FindFunction(vm, "StartFrame")) {
		QC_SetGInt(vm, G.self, 0);
		QC_SetGInt(vm, G.other, 0);
		QC_ExecuteByName(vm, "StartFrame");
	}
	SV_ClientsPreThink();
	SV_RunBotCommands();
	for (int e = 1; e < vm->num_edicts; e++) {
		if (vm->edicts[e].free)
			continue;
		if (QC_GFloat(vm, G.force_retouch))
			SV_LinkEdict(e, true);
		if (e <= svs.maxclients) {
			/* players move through their usercmds; only thinks run here */
			client_t *c = SV_ClientForEdict(e);
			if (c->state == CS_SPAWNED)
				run_think(e, (float)(sv.time + sv.frametime * 0.5));
			continue;
		}
		run_entity(e);
	}
	SV_ClientsPostThink();
	if (QC_GFloat(vm, G.force_retouch) > 0)
		QC_SetGFloat(vm, G.force_retouch, QC_GFloat(vm, G.force_retouch) - 1);
	if (QC_FindFunction(vm, "EndFrame")) {
		QC_SetGInt(vm, G.self, 0);
		QC_ExecuteByName(vm, "EndFrame");
	}
}

void SV_Frame(double dt)
{
	static double accum;
	if (!sv.active)
		return;
	SV_ReadPackets();
	if (!sv.active)
		return;
	accum += dt;
	if (accum > 0.25)
		accum = 0.25; /* after a stall, drop time rather than spiral */
	while (accum >= sv.frametime) {
		accum -= sv.frametime;
		if (!sv.paused) {
			SV_Physics();
			sv.time += sv.frametime;
			sv.frames++;
		}
		SV_ClientsAfterPhysics();
		QC_CollectGarbage(sv.vm, false);
	}
	SV_SendClientMessages();
}

/* commands registered with registercommand go to the progs' ConsoleCmd */
bool SV_ConsoleCommand(const char *line)
{
	int f;
	cmd_tokens_t *t;
	bool known = false;
	if (!sv.active || !sv.vm)
		return false;
	f = QC_FindFunction(sv.vm, "ConsoleCmd");
	if (!f)
		return false;
	t = Mem_Alloc(sizeof(*t));
	Cmd_TokenizeInto(t, line);
	for (int i = 0; t->argc && i < sv.numcommands; i++)
		if (!Str_ICmp(sv.commands[i], t->argv[0]))
			known = true;
	Mem_Free(t);
	if (!known)
		return false;
	sv.vm->globals[OFS_PARM(0)].i = QC_NewString(sv.vm, line);
	sv.vm->argc = 1;
	QC_SetGInt(sv.vm, G.self, 0);
	if (!QC_Execute(sv.vm, f))
		return true;
	return sv.vm->globals[OFS_RETURN].f != 0;
}

/* "sv <command>": hands the rest of the line to the progs' ConsoleCmd,
 * registered or not, as Nuclide's tools do with localcmd("sv ...") */
static void SV_Cmd_f(int argc, const char **argv)
{
	int f;
	if (argc < 2 || !sv.active || !sv.vm || !(f = QC_FindFunction(sv.vm, "ConsoleCmd"))) {
		Con_Printf("sv <command>: runs a command of the server game\n");
		return;
	}
	sv.vm->globals[OFS_PARM(0)].i = QC_NewString(sv.vm, Cmd_Args());
	sv.vm->argc = 1;
	QC_SetGInt(sv.vm, G.self, 0);
	if (QC_Execute(sv.vm, f) && sv.vm->globals[OFS_RETURN].f == 0)
		Con_Printf("server game: unknown command \"%s\"\n", argv[1]);
}

static void SV_Map_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("usage: map <name>\n");
		return;
	}
	/* a fresh map starts everyone's spawn parms over */
	for (int i = 0; i < svs.maxclients; i++)
		svs.clients[i].have_parms = false;
	SV_SpawnServer(argv[1]);
}

static void SV_Changelevel_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("usage: changelevel <name>\n");
		return;
	}
	SV_SaveSpawnParms();
	SV_SpawnServer(argv[1]);
}

static void SV_Status_f(int argc, const char **argv)
{
	int live = 0;
	if (!sv.active) {
		Con_Printf("no map running\n");
		return;
	}
	for (int e = 0; e < sv.vm->num_edicts; e++)
		live += !sv.vm->edicts[e].free;
	Con_Printf("map %s, time %.2f, %d frames, %d/%d entities, %d QC strings\n", sv.mapname, sv.time, sv.frames,
	           live, sv.vm->max_edicts, QC_LiveStrings());
}

static void SV_Edicts_f(int argc, const char **argv)
{
	if (!sv.active)
		return;
	for (int e = 0; e < sv.vm->num_edicts; e++) {
		if (sv.vm->edicts[e].free)
			continue;
		Con_Printf("%4d %s\n", e, QC_GetString(sv.vm, QC_EInt(sv.vm, e, F.classname)));
	}
}

/* "edict <n>": one entity's placement, for checking movement in scripted runs */
static void SV_Edict_f(int argc, const char **argv)
{
	int e = argc > 1 ? atoi(argv[1]) : 1;
	qcval_t *f;
	if (!sv.active || e < 0 || e >= sv.vm->num_edicts || sv.vm->edicts[e].free)
		return;
	f = QC_EdictFields(sv.vm, e);
	Con_Printf("edict %d %s origin %g %g %g velocity %g %g %g angles %g %g %g\n", e,
	           QC_GetString(sv.vm, QC_EInt(sv.vm, e, F.classname)), f[F.origin].f, f[F.origin + 1].f,
	           f[F.origin + 2].f, f[F.velocity].f, f[F.velocity + 1].f, f[F.velocity + 2].f, f[F.angles].f,
	           f[F.angles + 1].f, f[F.angles + 2].f);
}

/* "pr_trace <text>": prints entries to QC functions whose names contain it,
 * in every VM; with no argument, stops */
static void PrTrace_f(int argc, const char **argv)
{
	Str_Copy(qc_tracefilter, argc > 1 ? argv[1] : "", sizeof(qc_tracefilter));
}

void SV_Init(void)
{
	Cmd_Add("pr_trace", PrTrace_f);
	Cmd_Add("sv", SV_Cmd_f);
	sv_gravity = Cvar_Get("sv_gravity", "800", CVAR_SERVERINFO, "Downward acceleration for tossed entities");
	sv_maxvelocity = Cvar_Get("sv_maxvelocity", "2000", 0, "Velocity clamp per axis");
	sv_progs = Cvar_Get("sv_progs", "progs.dat", 0, "Server game module");
	pr_maxedicts = Cvar_Get("pr_maxedicts", "4096", 0, "Entity slots in the server game");
	pr_heapsize = Cvar_Get("pr_heapsize", "32", 0, "memalloc heap size in megabytes");
	sv_tickrate = Cvar_Get("sv_tickrate", "72", 0, "Server frames per second");
	Cvar_Get("mapname", "", CVAR_READONLY, NULL);
	Cmd_Add("map", SV_Map_f);
	Cmd_Add("changelevel", SV_Changelevel_f);
	SV_InitClients();
	Cmd_Add("status", SV_Status_f);
	Cmd_Add("edicts", SV_Edicts_f);
	Cmd_Add("edict", SV_Edict_f);
}
