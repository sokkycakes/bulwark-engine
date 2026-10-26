/* Bulwark engine - runstandardplayerphysics on the server.
 * ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/cvar.h"
#include "../common/pmove.h"


static int pm_passent;

static void sv_pm_trace(void *ctx, const float *start, const float *mins, const float *maxs, const float *end, pmtrace_t *out)
{
	svtrace_t tr;
	SV_Move(start, mins, maxs, end, MOVE_NORMAL, pm_passent, &tr);
	out->allsolid = tr.allsolid;
	out->startsolid = tr.startsolid;
	out->fraction = tr.fraction;
	VectorCopy(tr.endpos, out->endpos);
	VectorCopy(tr.normal, out->normal);
	out->ent = tr.ent;
}

static int sv_pm_contents(void *ctx, const float *p) { return SV_ContentsToQuake(SV_PointContentsBits(p)); }

/* movement settings come from cvars so clients can predict with the same
 * values; they are mirrored into serverinfo */
void SV_PMoveVars(pmvars_t *v)
{
	PM_DefaultVars(v);
	v->gravity = Cvar_Value("sv_gravity");
	v->maxspeed = Cvar_Value("sv_maxspeed");
	v->accelerate = Cvar_Value("sv_accelerate");
	v->airaccelerate = Cvar_Value("sv_airaccelerate");
	v->wateraccelerate = Cvar_Value("sv_wateraccelerate");
	v->friction = Cvar_Value("sv_friction");
	v->waterfriction = Cvar_Value("sv_waterfriction");
	v->stopspeed = Cvar_Value("sv_stopspeed");
	v->stepheight = Cvar_Value("sv_stepheight");
	v->jumpspeed = Cvar_Value("sv_jumpspeed");
	v->spectatormaxspeed = Cvar_Value("sv_spectatormaxspeed");
}

void SV_InitPMove(void)
{
	Cvar_Get("sv_maxspeed", "320", CVAR_SERVERINFO, "Player ground speed");
	Cvar_Get("sv_accelerate", "10", CVAR_SERVERINFO, "Ground acceleration");
	Cvar_Get("sv_airaccelerate", "10", CVAR_SERVERINFO, "Air acceleration (gain is capped at 30 units/s like Source)");
	Cvar_Get("sv_wateraccelerate", "10", CVAR_SERVERINFO, "Water acceleration");
	Cvar_Get("sv_friction", "4", CVAR_SERVERINFO, "Ground friction");
	Cvar_Get("sv_waterfriction", "1", CVAR_SERVERINFO, "Water friction");
	Cvar_Get("sv_stopspeed", "100", CVAR_SERVERINFO, "Speed below which friction stops a player outright");
	Cvar_Get("sv_stepheight", "18", CVAR_SERVERINFO, "Tallest step a player walks up");
	Cvar_Get("sv_jumpspeed", "268.3", CVAR_SERVERINFO, "Upward velocity of a jump");
	Cvar_Get("sv_spectatormaxspeed", "500", CVAR_SERVERINFO, "Free-fly speed");
}

/* Runs one input frame of standard movement on entity e of any VM, using
 * that VM's input_* globals. The caller picks the world with
 * SV_SetWorldContext. */
void W_RunPlayerPhysics(qcvm_t *vm, int e, const pmvars_t *vars)
{
	pmstate_t s;
	pmcmd_t cmd;
	pmworld_t world = {NULL, sv_pm_trace, sv_pm_contents};
	qcval_t *f;
	if (e <= 0 || e >= vm->num_edicts)
		return;
	f = QC_EdictFields(vm, e);
	memset(&s, 0, sizeof(s));
	VectorCopy(&f[vm->fofs.origin].f, s.origin);
	VectorCopy(&f[vm->fofs.velocity].f, s.velocity);
	VectorCopy(&f[vm->fofs.mins].f, s.mins);
	VectorCopy(&f[vm->fofs.maxs].f, s.maxs);
	s.flags = (int)f[vm->fofs.flags].f;
	s.movetype = (int)f[vm->fofs.movetype].f;
	s.gravity = vm->fofs.gravity >= 0 ? f[vm->fofs.gravity].f : 0;
	{
		int ms = QC_FieldOfs(vm, "maxspeed");
		s.maxspeed = ms >= 0 ? f[ms].f : 0;
	}
	memset(&cmd, 0, sizeof(cmd));
	cmd.msec = QC_GFloat(vm, QC_GlobalOfs(vm, "input_timelength"));
	{
		int a = QC_GlobalOfs(vm, "input_angles"), m = QC_GlobalOfs(vm, "input_movevalues");
		if (a >= 0)
			VectorCopy(&vm->globals[a].f, cmd.angles);
		if (m >= 0)
			VectorCopy(&vm->globals[m].f, cmd.movevalues);
	}
	cmd.buttons = (unsigned)QC_GFloat(vm, QC_GlobalOfs(vm, "input_buttons"));
	pm_passent = e;
	PM_Move(&s, &cmd, vars, &world);

	VectorCopy(s.origin, &f[vm->fofs.origin].f);
	VectorCopy(s.velocity, &f[vm->fofs.velocity].f);
	f[vm->fofs.flags].f = (float)s.flags;
	if (vm->fofs.groundentity >= 0)
		f[vm->fofs.groundentity].i = s.groundent > 0 ? s.groundent : 0;
	if (vm->fofs.waterlevel >= 0)
		f[vm->fofs.waterlevel].f = (float)s.waterlevel;
	if (vm->fofs.watertype >= 0)
		f[vm->fofs.watertype].f = (float)s.watertype;
	SV_LinkEdict(e, true);
}

void SV_RunStandardPlayerPhysics(int e)
{
	pmvars_t vars;
	SV_PMoveVars(&vars);
	W_RunPlayerPhysics(sv.vm, e, &vars);
}
