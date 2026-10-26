/* Bulwark engine - server-side builtins (world, precaches, info keys).
 * Networking builtins are accepted and dropped until the netcode exists.
 * ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/cvar.h"
#include "../common/cmd.h"

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

static void bi_setorigin(qcvm_t *vm)
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

static void bi_setsize(qcvm_t *vm)
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

static void bi_traceline(qcvm_t *vm)
{
	float zero[3] = {0, 0, 0};
	do_trace(vm, QC_PVEC(vm, 0), zero, zero, QC_PVEC(vm, 1), (int)QC_PFLOAT(vm, 2), QC_PENT(vm, 3));
}

static void bi_tracebox(qcvm_t *vm)
{
	do_trace(vm, QC_PVEC(vm, 0), QC_PVEC(vm, 1), QC_PVEC(vm, 2), QC_PVEC(vm, 3), (int)QC_PFLOAT(vm, 4), QC_PENT(vm, 5));
}

static void bi_pointcontents(qcvm_t *vm) { QC_ReturnFloat(vm, (float)SV_ContentsToQuake(SV_PointContentsBits(QC_PVEC(vm, 0)))); }

static void bi_droptofloor(qcvm_t *vm)
{
	QC_ReturnFloat(vm, SV_DropToFloor(QC_GInt(vm, G.self)) ? 1 : 0);
}

static void bi_checkbottom(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0);
	float *org = ev(vm, e, F.origin), end[3], zero[3] = {0, 0, 0};
	svtrace_t tr;
	VectorCopy(org, end);
	end[2] += QC_EFloat(vm, e, F.mins + 2) - 18;
	SV_Move(org, zero, zero, end, MOVE_NOMONSTERS, e, &tr);
	QC_ReturnFloat(vm, tr.fraction < 1 ? 1 : 0);
}

static void bi_walkmove(qcvm_t *vm)
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

static void bi_changeyaw(qcvm_t *vm)
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

static void bi_touchtriggers(qcvm_t *vm)
{
	int e = vm->argc >= 1 ? QC_PENT(vm, 0) : QC_GInt(vm, G.self);
	if (vm->argc >= 2) {
		float *org = ev(vm, e, F.origin);
		if (org)
			VectorCopy(QC_PVEC(vm, 1), org);
	}
	SV_LinkEdict(e, true);
}

static void bi_checkpvs(qcvm_t *vm) { QC_ReturnFloat(vm, 1); /* TODO: real PVS once vis data is loaded */ }
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
	QC_ReturnFloat(vm, (float)sv.numparticles);
}

static void bi_print_to_console(qcvm_t *vm, int first)
{
	strbuf_t sb = {0};
	for (int i = first; i < vm->argc; i++)
		SB_AppendStr(&sb, QC_PSTR(vm, i));
	if (sb.data)
		Con_Print(sb.data);
	SB_Free(&sb);
}

/* with no clients yet, client-directed text goes to the server console */
static void bi_bprint(qcvm_t *vm)
{
	/* the QW form takes a print level first */
	bool qw = vm->argc >= 2 && !((uint32_t)QC_PINT(vm, 0) & QC_STRING_RUNTIME) && QC_PFLOAT(vm, 0) < 16 && QC_PFLOAT(vm, 0) >= 0 &&
	          QC_PINT(vm, 0) != 0 && QC_PFLOAT(vm, 0) == (int)QC_PFLOAT(vm, 0);
	bi_print_to_console(vm, qw ? 1 : 0);
}
static void bi_sprint(qcvm_t *vm) { bi_print_to_console(vm, vm->argc >= 3 ? 2 : 1); }
static void bi_centerprint(qcvm_t *vm) { bi_print_to_console(vm, 1); }
static void bi_dropped(qcvm_t *vm) { (void)vm; }

static void bi_localsound(qcvm_t *vm) { (void)vm; }

static void bi_sound(qcvm_t *vm)
{
	const char *s = QC_PSTR(vm, 2);
	if (*s && !SV_SoundIndex(s, false))
		Con_DPrintf("sound: %s was not precached\n", s);
}

static void bi_forceinfokey(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0);
	if (e == 0)
		SV_InfoSet(sv.serverinfo, sizeof(sv.serverinfo), QC_PSTR(vm, 1), QC_PSTR(vm, 2));
	/* TODO: per-client userinfo */
}

static void bi_infokey(qcvm_t *vm)
{
	char buf[1024];
	int e = QC_PENT(vm, 0);
	const char *key = QC_PSTR(vm, 1);
	if (e != 0) {
		QC_ReturnInt(vm, 0);
		return;
	}
	SV_InfoGet(sv.serverinfo, key, buf, sizeof(buf));
	if (!buf[0])
		SV_InfoGet(sv.localinfo, key, buf, sizeof(buf));
	if (buf[0])
		QC_ReturnString(vm, buf);
	else
		QC_ReturnInt(vm, 0);
}

static void bi_infokeyf(qcvm_t *vm)
{
	char buf[1024];
	if (QC_PENT(vm, 0) != 0) {
		QC_ReturnFloat(vm, 0);
		return;
	}
	SV_InfoGet(sv.serverinfo, QC_PSTR(vm, 1), buf, sizeof(buf));
	QC_ReturnFloat(vm, (float)atof(buf));
}

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

static void bi_setserverkey(qcvm_t *vm) { SV_InfoSet(sv.serverinfo, sizeof(sv.serverinfo), QC_PSTR(vm, 0), QC_PSTR(vm, 1)); }

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
	snprintf(cmd, sizeof(cmd), "map \"%s\"\n", QC_PSTR(vm, 0));
	Cbuf_AddText(cmd);
}

static void bi_makestatic(qcvm_t *vm)
{
	/* static entities only matter to clients; drop the server copy */
	int e = QC_PENT(vm, 0);
	if (e > 0)
		QC_FreeEdict(vm, e, (float)sv.time);
}

static void bi_clienttype(qcvm_t *vm) { QC_ReturnFloat(vm, 3); /* CLIENTTYPE_NOTACLIENT until clients exist */ }
static void bi_return_world(qcvm_t *vm) { QC_ReturnInt(vm, 0); }

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

static void bi_frameforname(qcvm_t *vm) { QC_ReturnFloat(vm, -1); /* TODO: model framegroups */ }
static void bi_zero(qcvm_t *vm) { QC_ReturnFloat(vm, 0); }

static void bi_addprogs(qcvm_t *vm)
{
	/* TODO: multiple progs sharing one entity space */
	Con_DPrintf("addprogs(%s): not supported yet\n", QC_PSTR(vm, 0));
	QC_ReturnFloat(vm, -1);
}

static bool same_progs(qcvm_t *vm, float prnum)
{
	return prnum == 0 || prnum == -1 || prnum == -2;
}

static void bi_externvalue(qcvm_t *vm)
{
	qcdef_t *d;
	memset(QC_RETURN(vm), 0, sizeof(qcval_t) * 3);
	if (!same_progs(vm, QC_PFLOAT(vm, 0)))
		return;
	d = QC_FindGlobal(vm, QC_PSTR(vm, 1));
	if (d) {
		int words = (d->type & ~DEF_SAVEGLOBAL) == ev_vector ? 3 : 1;
		memcpy(QC_RETURN(vm), vm->globals + d->ofs, sizeof(qcval_t) * words);
	} else {
		QC_ReturnInt(vm, QC_FindFunction(vm, QC_PSTR(vm, 1)));
	}
}

static void bi_externset(qcvm_t *vm)
{
	qcdef_t *d;
	if (!same_progs(vm, QC_PFLOAT(vm, 0)))
		return;
	d = QC_FindGlobal(vm, QC_PSTR(vm, 2));
	if (d)
		vm->globals[d->ofs] = *QC_Parm(vm, 1);
}

static void bi_externcall(qcvm_t *vm)
{
	int f;
	int argc = vm->argc;
	if (!same_progs(vm, QC_PFLOAT(vm, 0)) || !(f = QC_FindFunction(vm, QC_PSTR(vm, 1)))) {
		QC_ReturnInt(vm, 0);
		return;
	}
	/* shift the remaining arguments down by two */
	for (int i = 2; i < argc && i < MAX_PARMS; i++)
		memcpy(QC_Parm(vm, i - 2), QC_Parm(vm, i), sizeof(qcval_t) * 3);
	vm->argc = argc - 2;
	QC_Execute(vm, f);
}

static void bi_setspawnparms(qcvm_t *vm) { (void)vm; }

/* no rigid-body physics yet; Nuclide falls back to its own movement */
static void bi_physics_supported(qcvm_t *vm) { QC_ReturnFloat(vm, 0); }

const qc_builtin_t sv_builtins[] = {
	{"physics_supported", bi_physics_supported},
	{"spawn", bi_spawn}, {"remove", bi_remove}, {"removeinstant", bi_remove},
	{"setorigin", bi_setorigin}, {"setsize", bi_setsize}, {"setmodel", bi_setmodel},
	{"setmodelindex", bi_setmodelindex}, {"precache_model", bi_precache_model},
	{"precache_model2", bi_precache_model}, {"precache_vwep_model", bi_precache_model},
	{"precache_sound", bi_precache_sound}, {"precache_sound2", bi_precache_sound},
	{"precache_file", bi_precache_file}, {"precache_file2", bi_precache_file},
	{"getmodelindex", bi_getmodelindex}, {"modelnameforindex", bi_modelnameforindex},
	{"getsoundindex", bi_getsoundindex}, {"soundnameforindex", bi_soundnameforindex},
	{"traceline", bi_traceline}, {"tracebox", bi_tracebox}, {"pointcontents", bi_pointcontents},
	{"droptofloor", bi_droptofloor}, {"checkbottom", bi_checkbottom}, {"walkmove", bi_walkmove},
	{"changeyaw", bi_changeyaw}, {"touchtriggers", bi_touchtriggers}, {"checkpvs", bi_checkpvs},
	{"aim", bi_aim}, {"lightstyle", bi_lightstyle}, {"lightstylestatic", bi_lightstyle},
	{"getlightstyle", bi_getlightstyle}, {"getlight", bi_getlight},
	{"particleeffectnum", bi_particleeffectnum}, {"pointparticles", bi_dropped},
	{"trailparticles", bi_dropped}, {"particle", bi_dropped}, {"effect", bi_dropped},
	{"sound", bi_sound}, {"pointsound", bi_dropped}, {"ambientsound", bi_dropped},
	{"localsound", bi_localsound}, {"bprint", bi_bprint}, {"sprint", bi_sprint},
	{"centerprint", bi_centerprint}, {"stuffcmd", bi_dropped}, {"stuffcmdflags", bi_dropped},
	{"clientcommand", bi_dropped}, {"multicast", bi_dropped}, {"WriteByte", bi_dropped},
	{"WriteChar", bi_dropped}, {"WriteShort", bi_dropped}, {"WriteLong", bi_dropped},
	{"WriteCoord", bi_dropped}, {"WriteAngle", bi_dropped}, {"WriteString", bi_dropped},
	{"WriteEntity", bi_dropped}, {"WriteFloat", bi_dropped}, {"WriteInt", bi_dropped},
	{"WriteUInt", bi_dropped}, {"WriteInt64", bi_dropped}, {"WriteUInt64", bi_dropped},
	{"WriteDouble", bi_dropped}, {"WriteUnterminatedString", bi_dropped},
	{"setsendneeded", bi_dropped}, {"forceinfokey", bi_forceinfokey}, {"infokey", bi_infokey},
	{"infokeyf", bi_infokeyf}, {"serverkey", bi_serverkey}, {"serverkeyfloat", bi_serverkeyfloat},
	{"setserverkey", bi_setserverkey}, {"setlocalinfo", bi_setlocalinfo},
	{"getlocalinfo", bi_getlocalinfo}, {"changelevel", bi_changelevel},
	{"makestatic", bi_makestatic}, {"clienttype", bi_clienttype}, {"checkclient", bi_return_world},
	{"spawnclient", bi_return_world}, {"dropclient", bi_dropped}, {"setpause", bi_dropped},
	{"registercommand", bi_registercommand}, {"getentitytoken", bi_getentitytoken},
	{"frameforname", bi_frameforname}, {"frameduration", bi_zero}, {"modelframecount", bi_zero},
	{"setattachment", bi_dropped}, {"setspawnparms", bi_setspawnparms}, {"logfrag", bi_dropped},
	{"addprogs", bi_addprogs}, {"externvalue", bi_externvalue}, {"externset", bi_externset},
	{"externcall", bi_externcall}, {"openportal", bi_dropped},
	{NULL, NULL}};
