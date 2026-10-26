/* Bulwark engine - hosting the client game (csprogs.dat): loading, entry
 * points, entity updates from snapshots, events, and the client builtins
 * that are not about drawing or sound. ISC licence, see LICENSE. */
#include "client.h"
#include "console.h"
#include "keys.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/digest.h"
#include "../common/fs.h"
#include "../common/pmove.h"
#include "../render/render.h"
#include "../server/sv_local.h"
#include "../sound/sound.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define F vm->fofs
#define G vm->gofs

msg_t *csqc_readmsg;

static struct {
	int player_localentnum, player_localnum, maxclients, clientcommandframe, servercommandframe, cltime, clframetime;
	int view_angles, numclientseats, servertime, serverprevtime, input_sequence, input_timelength, input_angles;
	int input_movevalues, input_buttons, input_impulse, input_servertime, input_clienttime, input_weapon;
	int input_cursor_screen, input_cursor_start, input_cursor_end, input_cursor_ent, intermission, gamespeed;
	int entnum; /* field */
	func_t Ent_Update, Ent_Remove, Parse_Event, UpdateView, InputEvent, Input_Frame, ConsoleCommand, Parse_Print;
	func_t Parse_CenterPrint, ServerInfoChanged, PlayerInfoChanged, WorldLoaded, Shutdown, Init, RendererRestarted;
} CS;

static char *registered[512];
static int numregistered;

/* --- world context for traces and movement in the client game --------- */

static bool cl_brushmodel(int mi, bspmap_t **bsp, int *sub)
{
	if (mi <= 0 || mi >= cl.nummodels || !cl.models[mi].bsp)
		return false;
	*bsp = cl.models[mi].bsp;
	*sub = cl.models[mi].submodel;
	return true;
}

static worldctx_t clw;

static worldctx_t *enter_world(void)
{
	clw.vm = cl.csqc;
	clw.world = cl.world;
	clw.brushmodel = cl_brushmodel;
	return SV_SetWorldContext(&clw);
}

#define WORLD_WRAP(name)                                                                                               \
	static void cl_##name(qcvm_t *vm)                                                                                  \
	{                                                                                                                  \
		worldctx_t *old = enter_world();                                                                               \
		W_bi_##name(vm);                                                                                               \
		SV_SetWorldContext(old);                                                                                       \
	}
WORLD_WRAP(setorigin)
WORLD_WRAP(setsize)
WORLD_WRAP(traceline)
WORLD_WRAP(tracebox)
WORLD_WRAP(pointcontents)
WORLD_WRAP(droptofloor)
WORLD_WRAP(checkbottom)
WORLD_WRAP(walkmove)
WORLD_WRAP(changeyaw)
WORLD_WRAP(touchtriggers)

static float info_float(const char *key, float def)
{
	char buf[64];
	extern const char *CL_ServerKey(const char *key, char *out, size_t size);
	CL_ServerKey(key, buf, sizeof(buf));
	return buf[0] ? (float)atof(buf) : def;
}

static void cl_runstandardplayerphysics(qcvm_t *vm)
{
	pmvars_t v;
	worldctx_t *old;
	PM_DefaultVars(&v);
	/* the server publishes its movement settings in serverinfo */
	v.gravity = info_float("sv_gravity", v.gravity);
	v.maxspeed = info_float("sv_maxspeed", v.maxspeed);
	v.accelerate = info_float("sv_accelerate", v.accelerate);
	v.airaccelerate = info_float("sv_airaccelerate", v.airaccelerate);
	v.wateraccelerate = info_float("sv_wateraccelerate", v.wateraccelerate);
	v.friction = info_float("sv_friction", v.friction);
	v.waterfriction = info_float("sv_waterfriction", v.waterfriction);
	v.stopspeed = info_float("sv_stopspeed", v.stopspeed);
	v.stepheight = info_float("sv_stepheight", v.stepheight);
	v.jumpspeed = info_float("sv_jumpspeed", v.jumpspeed);
	v.spectatormaxspeed = info_float("sv_spectatormaxspeed", v.spectatormaxspeed);
	old = enter_world();
	W_RunPlayerPhysics(vm, QC_PENT(vm, 0), &v);
	SV_SetWorldContext(old);
}

/* --- models ------------------------------------------------------------- */

static void set_size(qcvm_t *vm, int e, const float *mins, const float *maxs)
{
	qcval_t *f = QC_EdictFields(vm, e);
	for (int i = 0; i < 3; i++) {
		if (F.mins >= 0)
			f[F.mins + i].f = mins[i];
		if (F.maxs >= 0)
			f[F.maxs + i].f = maxs[i];
		if (F.size >= 0)
			f[F.size + i].f = maxs[i] - mins[i];
	}
}

static void cl_setmodelindex_common(qcvm_t *vm, int e, int idx)
{
	if (idx < 0 || idx >= cl.nummodels)
		idx = 0;
	if (F.model >= 0)
		QC_EdictFields(vm, e)[F.model].i = idx && cl.models[idx].name ? QC_NewString(vm, cl.models[idx].name) : 0;
	QC_SetEFloat(vm, e, F.modelindex, (float)idx);
	if (idx) {
		float mins[3], maxs[3];
		if (cl.models[idx].bsp)
			set_size(vm, e, cl.models[idx].mins, cl.models[idx].maxs);
		else if (R_ModelBounds(idx, mins, maxs))
			set_size(vm, e, mins, maxs);
	} else {
		float zero[3] = {0, 0, 0};
		set_size(vm, e, zero, zero);
	}
	{
		worldctx_t *old = enter_world();
		SV_LinkEdict(e, false);
		SV_SetWorldContext(old);
	}
}

static void cl_setmodel(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0);
	const char *m = QC_PSTR(vm, 1);
	cl_setmodelindex_common(vm, e, CL_ModelIndex(m, true));
}

static void cl_setmodelindex(qcvm_t *vm) { cl_setmodelindex_common(vm, QC_PENT(vm, 0), (int)QC_PFLOAT(vm, 1)); }

static void cl_precache_model(qcvm_t *vm)
{
	CL_ModelIndex(QC_PSTR(vm, 0), true);
	vm->globals[OFS_RETURN] = *QC_Parm(vm, 0);
}

static void cl_getmodelindex(qcvm_t *vm)
{
	bool queryonly = vm->argc >= 2 && QC_PFLOAT(vm, 1);
	QC_ReturnFloat(vm, (float)CL_ModelIndex(QC_PSTR(vm, 0), !queryonly));
}

static void cl_modelnameforindex(qcvm_t *vm)
{
	int i = (int)QC_PFLOAT(vm, 0);
	if (i > 0 && i < cl.nummodels && cl.models[i].name)
		QC_ReturnString(vm, cl.models[i].name);
	else
		QC_ReturnInt(vm, 0);
}

/* --- reading network data ------------------------------------------------ */

static msg_t empty_msg;
static msg_t *rd(void)
{
	if (!csqc_readmsg) {
		empty_msg.badread = true;
		return &empty_msg;
	}
	return csqc_readmsg;
}

static void cl_readbyte(qcvm_t *vm) { QC_ReturnFloat(vm, (float)(MSG_ReadByte(rd()) & 0xff)); }
static void cl_readchar(qcvm_t *vm) { QC_ReturnFloat(vm, (float)MSG_ReadChar(rd())); }
static void cl_readshort(qcvm_t *vm) { QC_ReturnFloat(vm, (float)MSG_ReadShort(rd())); }
static void cl_readlong(qcvm_t *vm) { QC_ReturnFloat(vm, (float)MSG_ReadLong(rd())); }
static void cl_readint(qcvm_t *vm) { QC_ReturnInt(vm, MSG_ReadLong(rd())); }
static void cl_readcoord(qcvm_t *vm) { QC_ReturnFloat(vm, MSG_ReadCoord(rd())); }
static void cl_readangle(qcvm_t *vm) { QC_ReturnFloat(vm, MSG_ReadAngle(rd())); }
static void cl_readfloat(qcvm_t *vm) { QC_ReturnFloat(vm, MSG_ReadFloat(rd())); }
static void cl_readstring(qcvm_t *vm) { QC_ReturnString(vm, MSG_ReadString(rd())); }

static void cl_readentitynum(qcvm_t *vm)
{
	int e = MSG_ReadEntity(rd());
	QC_ReturnFloat(vm, (float)e);
}

static void cl_readint64(qcvm_t *vm)
{
	int64_t v = MSG_ReadInt64(rd());
	vm->globals[OFS_RETURN].u = (uint32_t)v;
	vm->globals[OFS_RETURN + 1].u = (uint32_t)((uint64_t)v >> 32);
}

static void cl_readdouble(qcvm_t *vm)
{
	double d = MSG_ReadDouble(rd());
	uint64_t u;
	memcpy(&u, &d, 8);
	vm->globals[OFS_RETURN].u = (uint32_t)u;
	vm->globals[OFS_RETURN + 1].u = (uint32_t)(u >> 32);
}

/* --- entry points -------------------------------------------------------- */

static func_t fn(qcvm_t *vm, const char *name) { return QC_FindFunction(vm, name); }

static void set_frame_globals(qcvm_t *vm)
{
	QC_SetGFloat(vm, G.time, (float)cl.time);
	QC_SetGFloat(vm, CS.cltime, (float)cl.realtime);
	QC_SetGFloat(vm, CS.servertime, (float)cl.servertime);
	QC_SetGFloat(vm, CS.player_localentnum, (float)cl.playerent);
	QC_SetGFloat(vm, CS.player_localnum, (float)cl.playerslot);
	QC_SetGFloat(vm, CS.maxclients, (float)cl.maxclients);
	QC_SetGFloat(vm, CS.clientcommandframe, (float)cl.clientcommandframe);
	QC_SetGFloat(vm, CS.servercommandframe, (float)cl.servercommandframe);
	/* no seats until the player is in the game: Nuclide's View_Init, run
	 * from CSQC_RendererRestart during loading, sets up the seats at or
	 * above numclientseats, so it relies on this being 0 then */
	QC_SetGFloat(vm, CS.numclientseats, cl.state == ca_active ? 1 : 0);
	QC_SetGInt(vm, G.world, 0);
}

bool CSQC_Load(void)
{
	static const qc_builtin_t *tables[8];
	size_t len;
	char *data;
	qcvm_t *vm;
	CSQC_Shutdown();
	if (!cl.csprogsname[0]) {
		Con_Printf("^3the server has no client game; nothing will be drawn\n");
		return true;
	}
	data = FS_LoadFile(cl.csprogsname, &len);
	if (!data) {
		Con_Printf("couldn't load %s\n", cl.csprogsname);
		return false;
	}
	if (Digest_CRC16(data, len) != cl.csprogscrc)
		Con_Printf("^3%s differs from the server's copy; expect trouble\n", cl.csprogsname);
	tables[0] = cl_builtins;
	tables[1] = r_builtins;
	tables[2] = s_builtins;
	tables[3] = qc_builtins_core;
	tables[4] = qc_builtins_extra;
	tables[5] = NULL;
	vm = QC_Load(cl.csprogsname, data, len, QCVM_CLIENT, tables, 8192, 32 * 1024 * 1024);
	Mem_Free(data);
	if (!vm)
		return false;
	cl.csqc = vm;

	memset(&CS, -1, sizeof(CS));
	CS.player_localentnum = QC_GlobalOfs(vm, "player_localentnum");
	CS.player_localnum = QC_GlobalOfs(vm, "player_localnum");
	CS.maxclients = QC_GlobalOfs(vm, "maxclients");
	CS.clientcommandframe = QC_GlobalOfs(vm, "clientcommandframe");
	CS.servercommandframe = QC_GlobalOfs(vm, "servercommandframe");
	CS.cltime = QC_GlobalOfs(vm, "cltime");
	CS.clframetime = QC_GlobalOfs(vm, "clframetime");
	CS.view_angles = QC_GlobalOfs(vm, "view_angles");
	CS.numclientseats = QC_GlobalOfs(vm, "numclientseats");
	CS.servertime = QC_GlobalOfs(vm, "servertime");
	CS.serverprevtime = QC_GlobalOfs(vm, "serverprevtime");
	CS.input_sequence = QC_GlobalOfs(vm, "input_sequence");
	CS.input_timelength = QC_GlobalOfs(vm, "input_timelength");
	CS.input_angles = QC_GlobalOfs(vm, "input_angles");
	CS.input_movevalues = QC_GlobalOfs(vm, "input_movevalues");
	CS.input_buttons = QC_GlobalOfs(vm, "input_buttons");
	CS.input_impulse = QC_GlobalOfs(vm, "input_impulse");
	CS.input_servertime = QC_GlobalOfs(vm, "input_servertime");
	CS.input_clienttime = QC_GlobalOfs(vm, "input_clienttime");
	CS.input_weapon = QC_GlobalOfs(vm, "input_weapon");
	CS.input_cursor_screen = QC_GlobalOfs(vm, "input_cursor_screen");
	CS.input_cursor_start = QC_GlobalOfs(vm, "input_cursor_trace_start");
	CS.input_cursor_end = QC_GlobalOfs(vm, "input_cursor_trace_endpos");
	CS.input_cursor_ent = QC_GlobalOfs(vm, "input_cursor_entitynumber");
	CS.intermission = QC_GlobalOfs(vm, "intermission");
	CS.entnum = QC_FieldOfs(vm, "entnum");
	CS.Ent_Update = fn(vm, "CSQC_Ent_Update");
	CS.Ent_Remove = fn(vm, "CSQC_Ent_Remove");
	CS.Parse_Event = fn(vm, "CSQC_Parse_Event");
	CS.UpdateView = fn(vm, "CSQC_UpdateView");
	CS.InputEvent = fn(vm, "CSQC_InputEvent");
	CS.Input_Frame = fn(vm, "CSQC_Input_Frame");
	CS.ConsoleCommand = fn(vm, "CSQC_ConsoleCommand");
	CS.Parse_Print = fn(vm, "CSQC_Parse_Print");
	CS.Parse_CenterPrint = fn(vm, "CSQC_Parse_CenterPrint");
	CS.ServerInfoChanged = fn(vm, "CSQC_ServerInfoChanged");
	CS.PlayerInfoChanged = fn(vm, "CSQC_PlayerInfoChanged");
	CS.WorldLoaded = fn(vm, "CSQC_WorldLoaded");
	CS.Shutdown = fn(vm, "CSQC_Shutdown");
	CS.Init = fn(vm, "CSQC_Init");
	CS.RendererRestarted = fn(vm, "CSQC_RendererRestarted");

	/* the world entity, as the server sets it up */
	QC_ClearEdict(vm, 0);
	if (F.model >= 0)
		QC_EdictFields(vm, 0)[F.model].i = QC_NewString(vm, cl.worldpath);
	QC_SetEFloat(vm, 0, F.modelindex, 1);
	QC_SetEFloat(vm, 0, F.solid, SOLID_BSP);
	if (cl.world && F.mins >= 0) {
		set_size(vm, 0, cl.world->models[0].mins, cl.world->models[0].maxs);
	}
	if (G.mapname >= 0)
		vm->globals[G.mapname].i = QC_NewString(vm, cl.mapname);
	set_frame_globals(vm);

	cl.ents = Mem_Alloc(sizeof(clentity_t) * 65536);
	cl.numents = 65536;

	{
		func_t f = fn(vm, "init");
		if (f) {
			vm->globals[OFS_PARM(0)].f = 0;
			vm->argc = 1;
			QC_Execute(vm, f);
		}
	}
	if (CS.Init) {
		vm->globals[OFS_PARM(0)].f = 1; /* apilevel */
		vm->globals[OFS_PARM(1)].i = QC_NewString(vm, PROTOCOL_NAME);
		vm->globals[OFS_PARM(2)].f = PROTOCOL_VERSION;
		vm->argc = 3;
		QC_Execute(vm, CS.Init);
	}
	if (CS.RendererRestarted && R_Active()) {
		vm->globals[OFS_PARM(0)].i = QC_NewString(vm, "");
		vm->argc = 1;
		QC_Execute(vm, CS.RendererRestarted);
	}
	cl.csqc_initialised = true;
	return true;
}

void CSQC_WorldLoaded(void)
{
	qcvm_t *vm = cl.csqc;
	if (!vm)
		return;
	set_frame_globals(vm);
	if (CS.WorldLoaded)
		QC_Execute(vm, CS.WorldLoaded);
	cl.worldloaded = true;
}

void CSQC_Shutdown(void)
{
	if (!cl.csqc)
		return;
	if (CS.Shutdown)
		QC_Execute(cl.csqc, CS.Shutdown);
	QC_Free(cl.csqc);
	cl.csqc = NULL;
	cl.csqc_initialised = false;
	for (int i = 0; i < numregistered; i++)
		Mem_Free(registered[i]);
	numregistered = 0;
}

static int csqc_ent_for(int e, bool create, bool *isnew)
{
	qcvm_t *vm = cl.csqc;
	clentity_t *ce = &cl.ents[e];
	*isnew = false;
	if (ce->csqc_ent) {
		int ent = ce->csqc_ent;
		/* the game may have removed it itself */
		if (ent < vm->num_edicts && !vm->edicts[ent].free &&
		    (CS.entnum < 0 || (int)QC_EFloat(vm, ent, CS.entnum) == e))
			return ent;
		ce->csqc_ent = 0;
	}
	if (!create)
		return 0;
	ce->csqc_ent = QC_SpawnEdict(vm);
	QC_SetEFloat(vm, ce->csqc_ent, CS.entnum, (float)e);
	ce->numsamples = 0;
	*isnew = true;
	return ce->csqc_ent;
}

static void record_interp(int e, int ent)
{
	qcvm_t *vm = cl.csqc;
	clentity_t *ce = &cl.ents[e];
	interpsample_t *s;
	if (ce->numsamples == CL_INTERP_SAMPLES) {
		memmove(ce->samples, ce->samples + 1, sizeof(interpsample_t) * (CL_INTERP_SAMPLES - 1));
		ce->numsamples--;
	}
	s = &ce->samples[ce->numsamples++];
	s->time = cl.servertime;
	memcpy(s->origin, &QC_EdictFields(vm, ent)[F.origin], sizeof(float) * 3);
	memcpy(s->angles, &QC_EdictFields(vm, ent)[F.angles], sizeof(float) * 3);
}

void CSQC_ParseEntities(msg_t *m)
{
	qcvm_t *vm = cl.csqc;
	for (;;) {
		uint32_t v = MSG_ReadVarUInt(m);
		int e;
		if (!v || m->badread)
			break;
		e = (int)(v >> 1) - 1;
		if (e < 0 || e >= cl.numents || !vm) {
			if (!(v & 1)) { /* skip the payload */
				uint32_t len = MSG_ReadVarUInt(m);
				m->readpos = BW_MIN(m->readpos + len, m->cursize);
			}
			continue;
		}
		if (v & 1) {
			bool isnew;
			int ent = csqc_ent_for(e, false, &isnew);
			if (ent) {
				QC_SetGInt(vm, G.self, ent);
				if (CS.Ent_Remove)
					QC_Execute(vm, CS.Ent_Remove);
				else
					QC_FreeEdict(vm, ent, (float)cl.time);
			}
			cl.ents[e].csqc_ent = 0;
			cl.ents[e].numsamples = 0;
			continue;
		}
		{
			uint32_t len = MSG_ReadVarUInt(m);
			msg_t sub;
			bool isnew;
			int ent;
			if (MSG_Remaining(m) < len) {
				m->badread = true;
				break;
			}
			MSG_Init(&sub, m->data + m->readpos, len);
			sub.cursize = len;
			m->readpos += len;
			ent = csqc_ent_for(e, true, &isnew);
			set_frame_globals(vm);
			QC_SetGInt(vm, G.self, ent);
			vm->globals[OFS_PARM(0)].f = isnew ? 1 : 0;
			vm->argc = 1;
			csqc_readmsg = &sub;
			if (CS.Ent_Update)
				QC_Execute(vm, CS.Ent_Update);
			csqc_readmsg = NULL;
			if (sub.badread || sub.readpos != sub.cursize)
				Con_DPrintf("^3entity %d: CSQC_Ent_Update read %zu of %zu bytes\n", e, sub.readpos, sub.cursize);
			if (cl.ents[e].csqc_ent && !vm->edicts[cl.ents[e].csqc_ent].free)
				record_interp(e, cl.ents[e].csqc_ent);
		}
	}
}

void CSQC_ParseEvent(msg_t *m, size_t len)
{
	qcvm_t *vm = cl.csqc;
	msg_t sub;
	MSG_Init(&sub, m->data + m->readpos, len);
	sub.cursize = len;
	m->readpos += len;
	while (vm && MSG_Remaining(&sub) && !sub.badread) {
		int c = MSG_ReadByte(&sub);
		size_t before;
		if (c != SVC_CGAMEPACKET || !CS.Parse_Event) {
			Con_DPrintf("^3unhandled game message byte %d\n", c);
			break;
		}
		before = sub.readpos;
		set_frame_globals(vm);
		csqc_readmsg = &sub;
		QC_Execute(vm, CS.Parse_Event);
		csqc_readmsg = NULL;
		if (sub.readpos == before)
			break; /* read nothing: avoid spinning */
	}
}

void CSQC_Frame(int width, int height)
{
	qcvm_t *vm = cl.csqc;
	static double last;
	if (!vm || !CS.UpdateView || cl.state != ca_active)
		return;
	set_frame_globals(vm);
	QC_SetGFloat(vm, G.frametime, (float)(cl.realtime - last));
	QC_SetGFloat(vm, CS.clframetime, (float)(cl.realtime - last));
	last = cl.realtime;
	QC_SetGVec(vm, CS.view_angles, cl.viewangles);
	vm->globals[OFS_PARM(0)].f = (float)width;
	vm->globals[OFS_PARM(1)].f = (float)height;
	vm->globals[OFS_PARM(2)].f = 1; /* "notmenu": no menu progs yet */
	vm->argc = 3;
	QC_Execute(vm, CS.UpdateView);
	if (CS.view_angles >= 0)
		VectorCopy(&vm->globals[CS.view_angles].f, cl.viewangles);
	QC_CollectGarbage(vm, false);
}

bool CSQC_ConsoleCommand(const char *line)
{
	qcvm_t *vm = cl.csqc;
	cmd_tokens_t *t;
	bool known = false;
	if (!vm || !CS.ConsoleCommand)
		return false;
	t = Mem_Alloc(sizeof(*t));
	Cmd_TokenizeInto(t, line);
	for (int i = 0; t->argc && i < numregistered; i++)
		if (!Str_ICmp(registered[i], t->argv[0]))
			known = true;
	Mem_Free(t);
	if (!known)
		return false;
	set_frame_globals(vm);
	vm->globals[OFS_PARM(0)].i = QC_NewString(vm, line);
	vm->argc = 1;
	if (!QC_Execute(vm, CS.ConsoleCommand))
		return true;
	return vm->globals[OFS_RETURN].f != 0;
}

bool CSQC_InputEvent(int type, float a, float b, int device)
{
	qcvm_t *vm = cl.csqc;
	if (!vm || !CS.InputEvent || cl.state != ca_active)
		return false;
	set_frame_globals(vm);
	vm->globals[OFS_PARM(0)].f = (float)type;
	vm->globals[OFS_PARM(1)].f = a;
	vm->globals[OFS_PARM(2)].f = b;
	vm->globals[OFS_PARM(3)].f = (float)device;
	vm->argc = 4;
	if (!QC_Execute(vm, CS.InputEvent))
		return false;
	return vm->globals[OFS_RETURN].f != 0;
}

void CSQC_SetInputGlobals(qcvm_t *vm, const usercmd_t *u)
{
	QC_SetGFloat(vm, CS.input_sequence, (float)u->sequence);
	QC_SetGFloat(vm, CS.input_timelength, u->msec);
	QC_SetGVec(vm, CS.input_angles, u->angles);
	QC_SetGVec(vm, CS.input_movevalues, u->movevalues);
	QC_SetGFloat(vm, CS.input_buttons, (float)u->buttons);
	QC_SetGFloat(vm, CS.input_impulse, u->impulse);
	QC_SetGFloat(vm, CS.input_servertime, u->servertime_ms / 1000.0f);
	QC_SetGFloat(vm, CS.input_clienttime, (float)u->clienttime);
	if (CS.input_weapon >= 0) {
		vm->globals[CS.input_weapon].u = u->weapon;
		vm->globals[CS.input_weapon + 1].u = 0;
	}
	if (CS.input_cursor_screen >= 0) {
		float v[3] = {u->cursor_screen[0], u->cursor_screen[1], 0};
		QC_SetGVec(vm, CS.input_cursor_screen, v);
	}
	QC_SetGVec(vm, CS.input_cursor_start, u->cursor_start);
	QC_SetGVec(vm, CS.input_cursor_end, u->cursor_end);
	QC_SetGFloat(vm, CS.input_cursor_ent, (float)u->cursor_entity);
}

void CSQC_ReadInputGlobals(qcvm_t *vm, usercmd_t *u)
{
	if (CS.input_angles >= 0)
		VectorCopy(&vm->globals[CS.input_angles].f, u->angles);
	if (CS.input_movevalues >= 0)
		VectorCopy(&vm->globals[CS.input_movevalues].f, u->movevalues);
	u->buttons = (uint32_t)QC_GFloat(vm, CS.input_buttons);
	u->impulse = (uint8_t)QC_GFloat(vm, CS.input_impulse);
	if (CS.input_weapon >= 0)
		u->weapon = vm->globals[CS.input_weapon].u;
	if (CS.input_cursor_screen >= 0) {
		u->cursor_screen[0] = vm->globals[CS.input_cursor_screen].f;
		u->cursor_screen[1] = vm->globals[CS.input_cursor_screen + 1].f;
	}
	if (CS.input_cursor_start >= 0)
		VectorCopy(&vm->globals[CS.input_cursor_start].f, u->cursor_start);
	if (CS.input_cursor_end >= 0)
		VectorCopy(&vm->globals[CS.input_cursor_end].f, u->cursor_end);
	u->cursor_entity = (int)QC_GFloat(vm, CS.input_cursor_ent);
}

void CSQC_InputFrame(void)
{
	qcvm_t *vm = cl.csqc;
	if (!vm || !CS.Input_Frame)
		return;
	set_frame_globals(vm);
	QC_Execute(vm, CS.Input_Frame);
}

bool CSQC_Print(const char *text, int level)
{
	qcvm_t *vm = cl.csqc;
	if (!vm || !CS.Parse_Print)
		return false;
	vm->globals[OFS_PARM(0)].i = QC_NewString(vm, text);
	vm->globals[OFS_PARM(1)].f = (float)level;
	vm->argc = 2;
	QC_Execute(vm, CS.Parse_Print);
	return true;
}

bool CSQC_CenterPrint(const char *text)
{
	qcvm_t *vm = cl.csqc;
	if (!vm || !CS.Parse_CenterPrint)
		return false;
	vm->globals[OFS_PARM(0)].i = QC_NewString(vm, text);
	vm->argc = 1;
	QC_Execute(vm, CS.Parse_CenterPrint);
	return true;
}

void CSQC_ServerInfoChanged(void)
{
	if (cl.csqc && CS.ServerInfoChanged)
		QC_Execute(cl.csqc, CS.ServerInfoChanged);
}

void CSQC_PlayerInfoChanged(int slot)
{
	qcvm_t *vm = cl.csqc;
	if (!vm || !CS.PlayerInfoChanged)
		return;
	vm->globals[OFS_PARM(0)].f = (float)slot;
	vm->argc = 1;
	QC_Execute(vm, CS.PlayerInfoChanged);
}

/* --- builtins -------------------------------------------------------------- */

static void cl_getinputstate(qcvm_t *vm)
{
	usercmd_t u;
	uint32_t seq = (uint32_t)QC_PFLOAT(vm, 0);
	if (!CL_GetInputState(seq, &u)) {
		QC_ReturnFloat(vm, 0);
		return;
	}
	CSQC_SetInputGlobals(vm, &u);
	QC_ReturnFloat(vm, 1);
}

static void cl_sendevent(qcvm_t *vm)
{
	uint8_t buf[2048];
	msg_t m;
	const char *types = QC_PSTR(vm, 1);
	MSG_Init(&m, buf, sizeof(buf));
	MSG_WriteString(&m, QC_PSTR(vm, 0));
	MSG_WriteString(&m, types);
	for (int i = 0; types[i] && i < 6; i++) {
		qcval_t *p = QC_Parm(vm, 2 + i);
		switch (types[i]) {
		case 'f':
			MSG_WriteFloat(&m, p->f);
			break;
		case 'i':
			MSG_WriteLong(&m, p->i);
			break;
		case 'e': /* the server entity behind a client entity */
			MSG_WriteEntity(&m, CS.entnum >= 0 && p->i > 0 ? (int)QC_EFloat(vm, p->i, CS.entnum) : 0);
			break;
		case 'v':
			for (int k = 0; k < 3; k++)
				MSG_WriteFloat(&m, p[k].f);
			break;
		case 's':
			MSG_WriteString(&m, QC_GetString(vm, p->i));
			break;
		default:
			Con_Printf("^3sendevent: unknown argument type '%c'\n", types[i]);
			return;
		}
	}
	if (!m.overflowed)
		CL_SendEvent(&m);
}

extern const char *CL_ServerKey(const char *key, char *out, size_t size);

static void cl_serverkey(qcvm_t *vm)
{
	char buf[1024];
	const char *k = QC_PSTR(vm, 0);
	if (!strcmp(k, "ip"))
		QC_ReturnString(vm, NET_AdrToString(&cl.server));
	else if (!strcmp(k, "servername"))
		QC_ReturnString(vm, cl.servername);
	else if (!strcmp(k, "constate"))
		QC_ReturnString(vm, cl.state == ca_active ? "active" : cl.state >= ca_loading ? "connected" : "disconnected");
	else
		QC_ReturnString(vm, CL_ServerKey(k, buf, sizeof(buf)));
}

static void cl_serverkeyfloat(qcvm_t *vm)
{
	char buf[1024];
	const char *k = QC_PSTR(vm, 0);
	CL_ServerKey(k, buf, sizeof(buf));
	QC_ReturnFloat(vm, buf[0] ? (float)atof(buf) : (vm->argc > 1 ? QC_PFLOAT(vm, 1) : 0));
}

static void cl_serverkeyblob(qcvm_t *vm) { QC_ReturnInt(vm, 0); }

static const char *player_key(int slot, const char *key, char *buf, size_t size)
{
	if (slot < 0 || slot >= cl.maxclients)
		return "";
	if (!strcmp(key, "viewentity") || !strcmp(key, "*entnum")) {
		snprintf(buf, size, "%d", slot + 1);
		return buf;
	}
	return CL_PlayerKey(slot, key, buf, size);
}

static void cl_getplayerkeyvalue(qcvm_t *vm)
{
	char buf[1024];
	int slot = (int)QC_PFLOAT(vm, 0);
	if (slot < 0) /* negative numbers address players by entity: -1 is entity 1 */
		slot = -slot - 1;
	QC_ReturnString(vm, player_key(slot, QC_PSTR(vm, 1), buf, sizeof(buf)));
}

static void cl_getplayerkeyfloat(qcvm_t *vm)
{
	char buf[1024];
	int slot = (int)QC_PFLOAT(vm, 0);
	const char *v;
	if (slot < 0)
		slot = -slot - 1;
	v = player_key(slot, QC_PSTR(vm, 1), buf, sizeof(buf));
	QC_ReturnFloat(vm, v[0] ? (float)atof(v) : (vm->argc > 2 ? QC_PFLOAT(vm, 2) : 0));
}

static void cl_getplayerkeyblob(qcvm_t *vm) { QC_ReturnInt(vm, 0); }

static void cl_getlocaluserinfo(qcvm_t *vm)
{
	cvar_t *v = Cvar_Find(QC_PSTR(vm, 1));
	QC_ReturnString(vm, v && (v->flags & CVAR_USERINFO) ? v->string : "");
}

static void cl_setlocaluserinfo(qcvm_t *vm)
{
	const char *k = QC_PSTR(vm, 1), *val = QC_PSTR(vm, 2);
	cvar_t *v = Cvar_Find(k);
	if (!v)
		v = Cvar_Get(k, "", CVAR_USERINFO | CVAR_USERCREATED, NULL);
	v->flags |= CVAR_USERINFO;
	Cvar_Set(k, val);
}

static void cl_registercommand(qcvm_t *vm)
{
	const char *n = QC_PSTR(vm, 0);
	for (int i = 0; i < numregistered; i++)
		if (!Str_ICmp(registered[i], n))
			return;
	if (numregistered < 512)
		registered[numregistered++] = Mem_Strdup(n);
}

static void cl_isserver(qcvm_t *vm) { QC_ReturnFloat(vm, cl.listen ? 1 : 0); }
static void cl_isdemo(qcvm_t *vm) { QC_ReturnFloat(vm, 0); }
static void cl_clientstate(qcvm_t *vm) { QC_ReturnFloat(vm, cl.state == ca_active ? 2 : cl.state >= ca_loading ? 1 : 0); }

static void cl_getentity(qcvm_t *vm)
{
	/* engine-side (non-csqc) entity state: there is none in this engine */
	float zero[3] = {0, 0, 0};
	QC_ReturnVector(vm, zero);
}

static void cl_setpause(qcvm_t *vm)
{
	CL_SendStringCmd(QC_PFLOAT(vm, 0) ? "setpause 1" : "setpause 0");
}

static void cl_particleeffectnum(qcvm_t *vm)
{
	const char *n = QC_PSTR(vm, 0);
	for (int i = 1; i < CL_MAX_PARTICLES; i++)
		if (cl.particles[i] && !strcmp(cl.particles[i], n)) {
			QC_ReturnFloat(vm, (float)i);
			return;
		}
	for (int i = CL_MAX_PARTICLES - 1; i > 0; i--)
		if (!cl.particles[i]) {
			/* client-only effects fill the table from the top */
			cl.particles[i] = Mem_Strdup(n);
			QC_ReturnFloat(vm, (float)i);
			return;
		}
	QC_ReturnFloat(vm, 0);
}

/* --- keys --------------------------------------------------------------------- */

static void cl_getkeybind(qcvm_t *vm) { QC_ReturnString(vm, Key_GetBinding((int)QC_PFLOAT(vm, 0))); }
static void cl_setkeybind(qcvm_t *vm)
{
	Key_SetBinding((int)QC_PFLOAT(vm, 0), QC_PSTR(vm, 1));
	QC_ReturnFloat(vm, 1);
}
static void cl_keynumtostring(qcvm_t *vm) { QC_ReturnString(vm, Key_NameForNum((int)QC_PFLOAT(vm, 0))); }
static void cl_stringtokeynum(qcvm_t *vm) { QC_ReturnFloat(vm, (float)Key_NumForName(QC_PSTR(vm, 0))); }

static void cl_findkeysforcommand(qcvm_t *vm)
{
	const char *cmd = QC_PSTR(vm, 0);
	char out[64];
	int found[2] = {-1, -1}, n = 0;
	for (int k = 0; k < K_MAX && n < 2; k++)
		if (!Str_ICmp(Key_GetBinding(k), cmd))
			found[n++] = k;
	snprintf(out, sizeof(out), "%d %d", found[0], found[1]);
	QC_ReturnString(vm, out);
}

static void cl_findkeysforcommandex(qcvm_t *vm)
{
	const char *cmd = QC_PSTR(vm, 0);
	char out[256] = "";
	for (int k = 0; k < K_MAX; k++)
		if (!Str_ICmp(Key_GetBinding(k), cmd)) {
			if (out[0])
				Str_Cat(out, " ", sizeof(out));
			Str_Cat(out, Key_NameForNum(k), sizeof(out));
		}
	QC_ReturnString(vm, out);
}

static void cl_setsensitivityscaler(qcvm_t *vm) { (void)vm; }
static void cl_zero(qcvm_t *vm) { QC_ReturnInt(vm, 0); }
static void cl_nop(qcvm_t *vm) { (void)vm; }

static void cl_getlightstyle(qcvm_t *vm)
{
	int s = (int)QC_PFLOAT(vm, 0);
	QC_ReturnString(vm, s >= 0 && s < 256 && cl.lightstyles[s] ? cl.lightstyles[s] : "");
}

static void cl_lightstyle(qcvm_t *vm)
{
	int s = (int)QC_PFLOAT(vm, 0);
	if (s < 0 || s >= 256)
		return;
	Mem_Free(cl.lightstyles[s]);
	cl.lightstyles[s] = Mem_Strdup(QC_PSTR(vm, 1));
}

static void cl_anglesub(qcvm_t *vm)
{
	float out[3];
	for (int i = 0; i < 3; i++) {
		float d = fmodf(QC_PVEC(vm, 0)[i] - QC_PVEC(vm, 1)[i], 360.0f);
		if (d > 180)
			d -= 360;
		else if (d < -180)
			d += 360;
		out[i] = d;
	}
	QC_ReturnVector(vm, out);
}

const qc_builtin_t cl_builtins[] = {
	{"readbyte", cl_readbyte}, {"readchar", cl_readchar}, {"readshort", cl_readshort}, {"readlong", cl_readlong},
	{"readint", cl_readint}, {"readcoord", cl_readcoord}, {"readangle", cl_readangle}, {"readfloat", cl_readfloat},
	{"readstring", cl_readstring}, {"readentitynum", cl_readentitynum}, {"readint64", cl_readint64},
	{"readuint64", cl_readint64}, {"readdouble", cl_readdouble},
	{"getinputstate", cl_getinputstate}, {"sendevent", cl_sendevent},
	{"runstandardplayerphysics", cl_runstandardplayerphysics},
	{"serverkey", cl_serverkey}, {"serverkeyfloat", cl_serverkeyfloat}, {"serverkeyblob", cl_serverkeyblob},
	{"getplayerkeyvalue", cl_getplayerkeyvalue}, {"getplayerkeyfloat", cl_getplayerkeyfloat},
	{"getplayerkeyblob", cl_getplayerkeyblob}, {"getlocaluserinfo", cl_getlocaluserinfo},
	{"setlocaluserinfo", cl_setlocaluserinfo}, {"registercommand", cl_registercommand},
	{"isserver", cl_isserver}, {"isdemo", cl_isdemo}, {"clientstate", cl_clientstate}, {"getentity", cl_getentity},
	{"setpause", cl_setpause}, {"particleeffectnum", cl_particleeffectnum},
	{"setorigin", cl_setorigin}, {"setsize", cl_setsize}, {"setmodel", cl_setmodel}, {"setmodelindex", cl_setmodelindex},
	{"precache_model", cl_precache_model}, {"getmodelindex", cl_getmodelindex},
	{"modelnameforindex", cl_modelnameforindex}, {"traceline", cl_traceline}, {"tracebox", cl_tracebox},
	{"pointcontents", cl_pointcontents}, {"droptofloor", cl_droptofloor}, {"checkbottom", cl_checkbottom},
	{"walkmove", cl_walkmove}, {"changeyaw", cl_changeyaw}, {"touchtriggers", cl_touchtriggers},
	{"getkeybind", cl_getkeybind}, {"setkeybind", cl_setkeybind}, {"keynumtostring", cl_keynumtostring},
	{"keynumtostring_csqc", cl_keynumtostring}, {"keynumtostring_omgwtf", cl_keynumtostring},
	{"stringtokeynum", cl_stringtokeynum}, {"stringtokeynum_csqc", cl_stringtokeynum},
	{"findkeysforcommand", cl_findkeysforcommand}, {"findkeysforcommandex", cl_findkeysforcommandex},
	{"setsensitivityscaler", cl_setsensitivityscaler}, {"getlightstyle", cl_getlightstyle},
	{"lightstyle", cl_lightstyle}, {"anglesub", cl_anglesub},
	{"getstatf", cl_zero}, {"getstati", cl_zero}, {"getstats", cl_zero}, {"getplayerstat", cl_zero},
	{"setwatchpoint", cl_nop}, {"gp_rumble", cl_nop}, {"gp_rumbletriggers", cl_nop}, {"gp_setledcolor", cl_nop},
	{"gp_settriggerfx", cl_nop},
	{NULL, NULL}};
