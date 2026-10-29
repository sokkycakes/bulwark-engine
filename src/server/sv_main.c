/* Bulwark engine - server: progs lifecycle, map loading, entity spawning,
 * think and movement physics. ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../script/script.h"
#include "../common/cvar.h"
#include "../common/cmd.h"
#include "../common/fs.h"
#include "../scene/scene.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define F sv.vm->fofs
#define G sv.vm->gofs

server_t sv;
int sv_spawncount;

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

/* The info string other clients may see: keys starting with '_' are private
 * to the server (passwords, the player key), as in QuakeWorld. */
const char *SV_InfoPublic(const char *info, char *out, size_t outsize)
{
	const char *p = info;
	out[0] = 0;
	while (*p == '\\') {
		const char *k = p + 1, *ke = strchr(k, '\\'), *ve;
		if (!ke)
			break;
		ve = strchr(ke + 1, '\\');
		if (!ve)
			ve = ke + 1 + strlen(ke + 1);
		if (*k != '_') {
			size_t l = strlen(out), n = (size_t)(ve - p);
			if (l + n < outsize) {
				memcpy(out + l, p, n);
				out[l + n] = 0;
			}
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
static int spawn_entities(const char *data, bool world, int *first);

static void spawn_map_entities(const char *data)
{
	spawn_entities(data, true, NULL);
}

/* entity text spawned into the running map, as ent_create does; returns
 * how many blocks were spawned and the first one's number */
int SV_SpawnEntitiesFromText(const char *data, int *first) { return spawn_entities(data, false, first); }

static int spawn_entities(const char *data, bool world, int *first)
{
	qcvm_t *vm = sv.vm;
	int checkspawn = QC_FindFunction(vm, "CheckSpawn");
	int fullspawn = QC_GlobalOfs(vm, "__fullspawndata");
	int count = 0, failed = 0;
	bool map = world;
	if (first)
		*first = 0;

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
		if (first && !*first)
			*first = e;
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
	if (map)
		Con_Printf("%d map entities parsed, %d with no QC function for their classname\n", count, failed);
	return count;
}

/* --- lifecycle ------------------------------------------------------- */

static const qc_builtin_t *const sv_tables[] = {sv_client_builtins, sv_builtins, sv_route_builtins, sv_voxel_builtins, sv_stat_builtins, sv_link_builtins, sv_sql_builtins, qc_uri_builtins, script_builtins, qc_builtins_core, qc_builtins_extra, NULL};

void SV_Shutdown(void)
{
	SV_MVDMapChanged();
	if (sv.vm) {
		if (QC_FindFunction(sv.vm, "SV_Shutdown"))
			QC_ExecuteByName(sv.vm, "SV_Shutdown");
		SV_SQLShutdown();
		Script_LevelShutdown();
		QC_Free(sv.vm);
		SV_FreeRoutes();
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
	scenefile_t *scene = NULL;
	scene_entinfo_t sceneinfo;

	Str_Copy(keep_serverinfo, sv.serverinfo, sizeof(keep_serverinfo));
	Str_Copy(keep_localinfo, sv.localinfo, sizeof(keep_localinfo));
	SV_Shutdown();
	Str_Copy(sv.serverinfo, keep_serverinfo, sizeof(sv.serverinfo));
	Str_Copy(sv.localinfo, keep_localinfo, sizeof(sv.localinfo));

	Terrain_PrepareMap(mapname);
	Host_Loading("server_world", mapname, 0.0f);
	/* maps/<name>.scene wins over maps/<name>.bsp; its MapInstance names the world */
	snprintf(path, sizeof(path), "maps/%s.scene", mapname);
	if (FS_FileExists(path)) {
		scenefile_t *sf = Scene_Load(path);
		scene_prefabcache_t *prefabs = Scene_NewPrefabCache();
		if (!sf) {
			Scene_FreePrefabCache(prefabs);
			Host_Loading(NULL, NULL, 0);
			return false;
		}
		scene = Scene_Resolve(sf, Scene_PrefabCacheLoad, prefabs);
		Scene_Free(sf);
		Scene_FreePrefabCache(prefabs);
		Scene_MapInfo(scene, &sceneinfo);
		if (!sceneinfo.mapname[0]) {
			Con_Printf("%s has no MapInstance naming a BSP\n", path);
			Scene_Free(scene);
			Host_Loading(NULL, NULL, 0);
			return false;
		}
		Str_Copy(path, sceneinfo.mapname, sizeof(path));
	} else {
		/* then a BSP, then a void voxel map, then an FTEW world */
		snprintf(path, sizeof(path), "maps/%s.bsp", mapname);
		if (!FS_FileExists(path)) {
			char alt[160];
			snprintf(alt, sizeof(alt), "maps/%s.vxmap", mapname);
			if (FS_FileExists(alt))
				Str_Copy(path, alt, sizeof(path));
			else {
				snprintf(alt, sizeof(alt), "maps/%s.ftew", mapname);
				if (FS_FileExists(alt))
					Str_Copy(path, alt, sizeof(path));
			}
		}
	}
	sv.world = BSP_Load(path);
	if (!sv.world) {
		Con_Printf("couldn't load map %s\n", path);
		Scene_Free(scene);
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
		Scene_Free(scene);
		Host_Loading(NULL, NULL, 0);
		return false;
	}
	vm = QC_Load(sv_progs->string, progs, len, QCVM_SERVER, sv_tables, BW_MAX(64, pr_maxedicts->ivalue),
	             (size_t)BW_MAX(1, pr_heapsize->ivalue) * 1024 * 1024);
	Mem_Free(progs);
	if (!vm) {
		Scene_Free(scene);
		Host_Loading(NULL, NULL, 0);
		return false;
	}
	vm->host = &sv;
	vm->modelname = SV_ModelName;
	sv.vm = vm;
	Script_LevelInit(vm);
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

	/* maps/<name>.ent, when present, replaces the map's own entity lump */
	{
		char entpath[160];
		size_t entlen;
		char *ents;
		snprintf(entpath, sizeof(entpath), "maps/%s.ent", mapname);
		if ((ents = FS_LoadFile(entpath, &entlen))) {
			Con_Printf("using entities from %s\n", entpath);
			Mem_Free(sv.world->entities);
			sv.world->entities = ents;
		}
	}
	Script_LevelPreSpawn();
	Host_Loading("server_entities", mapname, 0.15f);
	if (scene) {
		strbuf_t ents = {0};
		Scene_BuildEntities(scene, sv.world->entities, &ents, &sceneinfo);
		Scene_Free(scene);
		Con_Printf("scene %s: %d entities from the scene, %d components with no entity\n", mapname,
		           sceneinfo.numentities, sceneinfo.numskipped);
		spawn_map_entities(ents.data ? ents.data : "");
		SB_Free(&ents);
	} else {
		spawn_map_entities(sv.world->entities);
	}
	sv.spawning = false;
	/* only slots freed while the map loaded skip the reuse delay; the
	   server clock starts at 1, so a fixed "before 2 seconds" would also
	   let a client that joins quickly reuse a slot some stale reference
	   still points at */
	vm->quick_reuse_before = (float)sv.time + 0.001f;
	sv_spawncount++;
	Script_LevelSpawned();
	SV_ClientsMapChanged();
	SV_MVDMapSpawned();
	SV_MasterMapSpawned();
	QC_CollectGarbage(vm, true);
	Con_Printf("map %s: %d entities in use, %d models, %d sounds precached\n", mapname,
	           vm->num_edicts, sv.nummodels - 1, sv.numsounds - 1);
	return true;
}

/* --- physics --------------------------------------------------------- */

/* The movement below runs for whichever module's entities are current: the
 * server's from SV_Physics, or the client game's from W_RunEntityPhysics
 * (with SV_SetWorldContext pointing traces at the client's world). */
static qcvm_t *pv;
static double ptime;
static float pdt, pgrav;
static bool pmove_ents;
#define PF pv->fofs
#define PG pv->gofs

static float *ev(int e, int ofs) { return ofs < 0 ? NULL : &QC_EdictFields(pv, e)[ofs].f; }

static bool run_think(int e, float thinktime_limit)
{
	qcvm_t *vm = pv;
	float thinktime = QC_EFloat(vm, e, PF.nextthink);
	func_t think;
	if (thinktime <= 0 || thinktime > thinktime_limit)
		return true;
	if (thinktime < ptime)
		thinktime = (float)ptime;
	QC_SetEFloat(vm, e, PF.nextthink, 0);
	think = QC_EInt(vm, e, PF.think);
	if (!think)
		return true;
	QC_SetGFloat(vm, PG.time, thinktime);
	QC_SetGInt(vm, PG.self, e);
	QC_SetGInt(vm, PG.other, 0);
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
	qcvm_t *vm = pv;
	int oldself = QC_GInt(vm, PG.self), oldother = QC_GInt(vm, PG.other);
	func_t t = QC_EInt(vm, e, PF.touch);
	if (t && QC_EFloat(vm, e, PF.solid) != SOLID_NOT) {
		QC_SetGInt(vm, PG.self, e);
		QC_SetGInt(vm, PG.other, other);
		QC_Execute(vm, t);
	}
	if (other > 0 && !vm->edicts[other].free) {
		t = QC_EInt(vm, other, PF.touch);
		if (t && QC_EFloat(vm, other, PF.solid) != SOLID_NOT) {
			QC_SetGInt(vm, PG.self, other);
			QC_SetGInt(vm, PG.other, e);
			QC_Execute(vm, t);
		}
	}
	QC_SetGInt(vm, PG.self, oldself);
	QC_SetGInt(vm, PG.other, oldother);
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
	float *org = ev(e, PF.origin), *vel = ev(e, PF.velocity), *ang = ev(e, PF.angles), *avel = ev(e, PF.avelocity);
	float end[3], dt = pdt;
	int flags = (int)QC_EFloat(pv, e, PF.flags);
	svtrace_t tr;
	if (!org || !vel)
		return;
	if ((flags & FL_ONGROUND) && movetype != MOVETYPE_FLY && movetype != MOVETYPE_FLYMISSILE) {
		if (vel[0] == 0 && vel[1] == 0 && vel[2] == 0)
			return;
		QC_SetEFloat(pv, e, PF.flags, (float)(flags & ~FL_ONGROUND));
	}
	clamp_velocity(vel);
	if (movetype == MOVETYPE_TOSS || movetype == MOVETYPE_BOUNCE || movetype == MOVETYPE_PHYSICS) {
		float grav = PF.gravity >= 0 && QC_EFloat(pv, e, PF.gravity) ? QC_EFloat(pv, e, PF.gravity) : 1;
		vel[2] -= grav * pgrav * dt;
	}
	if (ang && avel)
		for (int i = 0; i < 3; i++)
			ang[i] += avel[i] * dt;
	for (int i = 0; i < 3; i++)
		end[i] = org[i] + vel[i] * dt;
	SV_Move(org, ev(e, PF.mins), ev(e, PF.maxs), end, movetype == MOVETYPE_FLYMISSILE ? MOVE_MISSILE : MOVE_NORMAL, e, &tr);
	VectorCopy(tr.endpos, org);
	SV_LinkEdict(e, true);
	if (pv->edicts[e].free || tr.fraction == 1)
		return;
	if (tr.allsolid) {
		/* stuck: stop rather than tunnel */
		VectorClear(vel);
		return;
	}
	/* MOVETYPE_BOUNCE keeps half the speed into the surface, or the
	 * entity's .bouncefactor of it, and comes to rest on a floor below
	 * 60 units/s upward, or .bouncestop times the gravity it falls with
	 * (the DarkPlaces/FTE extension fields; 0 means unset) */
	float bounce = 0.5f, stop = 60;
	if (movetype == MOVETYPE_BOUNCE) {
		float bf = PF.bouncefactor >= 0 ? QC_EFloat(pv, e, PF.bouncefactor) : 0;
		float bs = PF.bouncestop >= 0 ? QC_EFloat(pv, e, PF.bouncestop) : 0;
		float grav = PF.gravity >= 0 && QC_EFloat(pv, e, PF.gravity) ? QC_EFloat(pv, e, PF.gravity) : 1;
		if (bf > 0)
			bounce = bf;
		if (bs > 0)
			stop = bs * grav * pgrav;
	}
	clip_velocity(vel, tr.normal, vel, (movetype == MOVETYPE_BOUNCE) ? 1.0f + bounce : 1.0f);
	if (tr.normal[2] > 0.7f && movetype != MOVETYPE_FLY && movetype != MOVETYPE_FLYMISSILE) {
		if (vel[2] < stop || movetype != MOVETYPE_BOUNCE) {
			QC_SetEFloat(pv, e, PF.flags, (float)((int)QC_EFloat(pv, e, PF.flags) | FL_ONGROUND));
			QC_SetEInt(pv, e, PF.groundentity, tr.ent);
			VectorClear(vel);
			if (avel)
				VectorClear(avel);
		}
	}
	/* touch functions read the hit from the trace globals: Nuclide's
	 * projectiles take trace_endpos and trace_plane_normal as the impact */
	SV_SetTraceGlobals(&tr);
	impact(e, tr.ent);
}

/* Moves what rides on or stands in the way of a brush mover by the same
 * amount. Riders are entities on the ground of the pusher; anything else is
 * pushed when the moved brushes now overlap it. If a pushed entity would
 * end up inside something, everything goes back and the pusher's blocked
 * function runs. Only translation pushes; turning movers carry nothing. */
static bool push_entities(int pusher, const float *move)
{
	qcvm_t *vm = pv;
	int moved[512], nmoved = 0;
	float saved[512][3];
	float *porg = ev(pusher, PF.origin), *pmin = ev(pusher, PF.absmin), *pmax = ev(pusher, PF.absmax);
	float lo[3], hi[3];
	for (int i = 0; i < 3; i++) {
		lo[i] = pmin[i] + BW_MIN(move[i], 0);
		hi[i] = pmax[i] + BW_MAX(move[i], 0);
	}
	for (int i = 0; i < 3; i++)
		porg[i] += move[i];
	SV_LinkEdict(pusher, false);
	for (int e = 1; e < vm->num_edicts; e++) {
		int mt, solid, flags;
		float *org, *amin, *amax;
		bool riding;
		svtrace_t tr;
		if (vm->edicts[e].free || e == pusher)
			continue;
		mt = (int)QC_EFloat(vm, e, PF.movetype);
		solid = (int)QC_EFloat(vm, e, PF.solid);
		if (mt == MOVETYPE_PUSH || mt == MOVETYPE_NONE || mt == MOVETYPE_NOCLIP || mt == MOVETYPE_FOLLOW)
			continue;
		if (solid == SOLID_NOT || solid == SOLID_TRIGGER || solid == SOLID_BSP || solid == SOLID_BSPTRIGGER)
			continue;
		org = ev(e, PF.origin);
		amin = ev(e, PF.absmin);
		amax = ev(e, PF.absmax);
		if (!org || !amin || !amax)
			continue;
		if (amin[0] > hi[0] || amin[1] > hi[1] || amin[2] > hi[2] || amax[0] < lo[0] || amax[1] < lo[1] ||
		    amax[2] < lo[2])
			continue;
		flags = (int)QC_EFloat(vm, e, PF.flags);
		riding = (flags & FL_ONGROUND) && QC_EInt(vm, e, PF.groundentity) == pusher;
		if (!riding && !SV_InsideBrushEntity(pusher, e))
			continue;
		if (nmoved < 512) {
			moved[nmoved] = e;
			VectorCopy(org, saved[nmoved]);
			nmoved++;
		}
		for (int i = 0; i < 3; i++)
			org[i] += move[i];
		if (!riding)
			QC_SetEFloat(vm, e, PF.flags, (float)(flags & ~FL_ONGROUND));
		SV_LinkEdict(e, false);
		SV_Move(org, ev(e, PF.mins), ev(e, PF.maxs), org, MOVE_NORMAL, e, &tr);
		if (!tr.startsolid)
			continue;
		/* a rider pressed against something only stays behind */
		if (riding && !SV_InsideBrushEntity(pusher, e)) {
			VectorCopy(saved[nmoved - 1], org);
			SV_LinkEdict(e, false);
			continue;
		}
		/* blocked: put everything back */
		for (int k = 0; k < nmoved; k++) {
			VectorCopy(saved[k], ev(moved[k], PF.origin));
			SV_LinkEdict(moved[k], false);
		}
		for (int i = 0; i < 3; i++)
			porg[i] -= move[i];
		SV_LinkEdict(pusher, false);
		{
			func_t b = QC_EInt(vm, pusher, PF.blocked);
			if (b) {
				int oldself = QC_GInt(vm, PG.self), oldother = QC_GInt(vm, PG.other);
				QC_SetGInt(vm, PG.self, pusher);
				QC_SetGInt(vm, PG.other, e);
				QC_Execute(vm, b);
				QC_SetGInt(vm, PG.self, oldself);
				QC_SetGInt(vm, PG.other, oldother);
			}
		}
		return false;
	}
	for (int k = 0; k < nmoved; k++)
		SV_LinkEdict(moved[k], true);
	return true;
}

/* brush movers: move, pushing what is in the way, and advance ltime */
static void physics_pusher(int e)
{
	float *org = ev(e, PF.origin), *vel = ev(e, PF.velocity), *ang = ev(e, PF.angles), *avel = ev(e, PF.avelocity);
	float ltime = QC_EFloat(pv, e, PF.ltime), oldltime = ltime;
	float thinktime = QC_EFloat(pv, e, PF.nextthink);
	float movetime = pdt;
	if (thinktime < ltime + pdt) {
		movetime = thinktime - ltime;
		if (movetime < 0)
			movetime = 0;
	}
	if (movetime > 0 && org && vel) {
		float move[3];
		bool pushed = true;
		for (int i = 0; i < 3; i++)
			move[i] = vel[i] * movetime;
		if ((move[0] || move[1] || move[2]) && QC_EFloat(pv, e, PF.solid) == SOLID_BSP)
			pushed = push_entities(e, move);
		else
			for (int i = 0; i < 3; i++)
				org[i] += move[i];
		if (pushed) {
			if (ang && avel)
				for (int i = 0; i < 3; i++)
					ang[i] += avel[i] * movetime;
			QC_SetEFloat(pv, e, PF.ltime, ltime + movetime);
			SV_LinkEdict(e, false);
		}
	}
	thinktime = QC_EFloat(pv, e, PF.nextthink);
	if (thinktime > oldltime && thinktime <= QC_EFloat(pv, e, PF.ltime)) {
		func_t think = QC_EInt(pv, e, PF.think);
		QC_SetEFloat(pv, e, PF.nextthink, 0);
		QC_SetGFloat(pv, PG.time, (float)ptime);
		QC_SetGInt(pv, PG.self, e);
		QC_SetGInt(pv, PG.other, 0);
		if (think)
			QC_Execute(pv, think);
	}
}

static void physics_noclip(int e)
{
	float *org = ev(e, PF.origin), *vel = ev(e, PF.velocity), *ang = ev(e, PF.angles), *avel = ev(e, PF.avelocity);
	if (!org || !vel)
		return;
	for (int i = 0; i < 3; i++) {
		org[i] += vel[i] * pdt;
		if (ang && avel)
			ang[i] += avel[i] * pdt;
	}
	SV_LinkEdict(e, false);
}

static void run_entity(int e)
{
	qcvm_t *vm = pv;
	int movetype = (int)QC_EFloat(vm, e, PF.movetype);
	func_t custom = QC_EInt(vm, e, PF.customphysics);
	float limit = (float)(ptime + pdt * 0.5);

	if (!pmove_ents && !custom) {
		run_think(e, limit);
		return;
	}
	if (custom) {
		/* the progs replaces engine movement for this entity */
		QC_SetGInt(vm, PG.self, e);
		QC_SetGInt(vm, PG.other, 0);
		QC_SetGFloat(vm, PG.time, (float)ptime);
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
		if (!((int)QC_EFloat(vm, e, PF.flags) & FL_ONGROUND))
			physics_toss(e, MOVETYPE_TOSS);
		run_think(e, limit);
		return;
	case MOVETYPE_WALK: /* player movement is run by the progs' own pmove */
	default:
		run_think(e, limit);
		return;
	}
}

void W_RunEntityPhysics(qcvm_t *vm, int e, double time, float frametime, float gravity, bool move)
{
	qcvm_t *oldpv = pv;
	double oldtime = ptime;
	float olddt = pdt, oldgrav = pgrav;
	bool oldmove = pmove_ents;
	pv = vm;
	ptime = time;
	pdt = frametime;
	pgrav = gravity;
	pmove_ents = move;
	run_entity(e);
	pv = oldpv;
	ptime = oldtime;
	pdt = olddt;
	pgrav = oldgrav;
	pmove_ents = oldmove;
}

void SV_Physics(void)
{
	qcvm_t *vm = sv.vm;
	pv = sv.vm;
	ptime = sv.time;
	pdt = sv.frametime;
	pgrav = sv_gravity->value;
	pmove_ents = true;
	QC_SetGFloat(vm, G.time, (float)sv.time);
	QC_SetGFloat(vm, G.frametime, sv.frametime);
	if (QC_FindFunction(vm, "StartFrame")) {
		QC_SetGInt(vm, G.self, 0);
		QC_SetGInt(vm, G.other, 0);
		QC_ExecuteByName(vm, "StartFrame");
	}
	SV_RunRoutes();
	SV_ClientsPreThink();
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
	Script_Frame(sv.time);
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
	SV_LinkReadPackets();
	if (!sv.active)
		return;
	accum += dt;
	if (accum > 0.25)
		accum = 0.25; /* after a stall, drop time rather than spiral */
	while (accum >= sv.frametime) {
		accum -= sv.frametime;
		if (!sv.paused) {
			SV_CompatFrame();
			SV_Physics();
			sv.time += sv.frametime;
			sv.frames++;
		}
		SV_ClientsAfterPhysics();
		QC_CollectGarbage(sv.vm, false);
	}
	SV_SendClientMessages();
	SV_LinkSend();
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
	if (SV_SpawnServer(argv[1]))
		SV_SaveLevelChanged();
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

static int prof_cmp(const void *a, const void *b)
{
	uint64_t x = (*(qcfunc_t *const *)a)->profile, y = (*(qcfunc_t *const *)b)->profile;
	return x < y ? 1 : x > y ? -1 : 0;
}

/* "pr_profile start" counts the statements each server QC function runs;
 * "pr_profile [n]" prints the n busiest (default 20), then starts over */
static void PrProfile_f(int argc, const char **argv)
{
	qcvm_t *vm = sv.vm;
	qcfunc_t **list;
	uint64_t total = 0;
	int n = 0, max = 0, show = 20;
	if (!vm) {
		Con_Printf("no map running\n");
		return;
	}
	if (argc > 1 && !strcmp(argv[1], "start")) {
		qc_profiling = true;
		for (int p = 0; p < vm->numprogs; p++)
			for (int i = 0; i < vm->progs[p].numfunctions; i++)
				vm->progs[p].functions[i].profile = 0;
		return;
	}
	if (argc > 1)
		show = atoi(argv[1]);
	for (int p = 0; p < vm->numprogs; p++)
		max += vm->progs[p].numfunctions;
	list = Mem_Alloc(sizeof(*list) * (size_t)BW_MAX(1, max));
	for (int p = 0; p < vm->numprogs; p++)
		for (int i = 0; i < vm->progs[p].numfunctions; i++) {
			qcfunc_t *f = &vm->progs[p].functions[i];
			if (f->profile) {
				list[n++] = f;
				total += f->profile;
			}
		}
	qsort(list, (size_t)n, sizeof(*list), prof_cmp);
	Con_Printf("%llu statements\n", (unsigned long long)total);
	for (int i = 0; i < n && i < show; i++)
		Con_Printf("%5.1f%% %12llu %s\n", 100.0 * (double)list[i]->profile / (double)BW_MAX(1, total),
		           (unsigned long long)list[i]->profile, list[i]->name);
	for (int i = 0; i < n; i++)
		list[i]->profile = 0;
	Mem_Free(list);
}

void SV_Init(void)
{
	Cmd_Add("pr_trace", PrTrace_f);
	Cmd_Add("pr_profile", PrProfile_f);
	Cmd_Add("sv", SV_Cmd_f);
	Script_Init();
	sv_gravity = Cvar_Get("sv_gravity", "800", CVAR_SERVERINFO, "Downward acceleration for tossed entities");
	sv_maxvelocity = Cvar_Get("sv_maxvelocity", "2000", 0, "Velocity clamp per axis");
	sv_progs = Cvar_Get("sv_progs", "progs.dat", 0, "Server game module");
	pr_maxedicts = Cvar_Get("pr_maxedicts", BW_SV_MAXEDICTS, 0, "Entity slots in the server game");
	pr_heapsize = Cvar_Get("pr_heapsize", BW_SV_HEAP_MB, 0, "memalloc heap size in megabytes");
	sv_tickrate = Cvar_Get("sv_tickrate", "72", 0, "Server frames per second");
	Cvar_Get("mapname", "", CVAR_READONLY, NULL);
	Cmd_Add("map", SV_Map_f);
	Cmd_Add("changelevel", SV_Changelevel_f);
	SV_InitClients();
	SV_LinkInit();
	Cmd_Add("status", SV_Status_f);
	Cmd_Add("edicts", SV_Edicts_f);
	Cmd_Add("edict", SV_Edict_f);
	SV_InitCommands();
	SV_InitSaves();
	SV_InitCompat();
	SV_InitAdmin();
	SV_InitRoutes();
	SV_InitMVD();
	SV_InitMaster();
	SV_InitDownloads();
	SV_InitVoip();
	SV_InitSQL();
	SV_InitCompile();
}
