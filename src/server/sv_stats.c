/* Bulwark engine - player stats: numbered values each client gets for its
 * own player (health, ammo, the items bits...), read by CSQC's getstat*
 * builtins and the teamplay macros. The first 32 come from the standard
 * Quake fields and globals; QC adds its own from 32 up with clientstat
 * (a field of the player), globalstat or pointerstat (a global), and the
 * DarkPlaces name addstat. Changes go out reliably with svc_stats.
 * ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/cmd.h"

#include <string.h>

enum { EVS_STRING = 1, EVS_FLOAT = 2, EVS_ENTITY = 4, EVS_INTEGER = 8 };
enum { SRC_NONE, SRC_FIELD, SRC_GLOBAL };

typedef struct {
	int src, type, ofs;
} statdef_t;

static statdef_t defs[MAX_CL_STATS];
static qcvm_t *defs_vm; /* the progs the definitions belong to */

/* standard stat numbers (the STAT_ constants of the QC extension headers) */
enum {
	STAT_HEALTH = 0, STAT_WEAPONMODELI = 2, STAT_AMMO = 3, STAT_ARMOR = 4, STAT_WEAPONFRAME = 5, STAT_SHELLS = 6,
	STAT_NAILS = 7, STAT_ROCKETS = 8, STAT_CELLS = 9, STAT_ACTIVEWEAPON = 10, STAT_TOTALSECRETS = 11,
	STAT_TOTALMONSTERS = 12, STAT_FOUNDSECRETS = 13, STAT_KILLEDMONSTERS = 14, STAT_ITEMS = 15, STAT_VIEWHEIGHT = 16,
	STAT_VIEW2 = 20, STAT_VIEWZOOM = 21
};

static void check_vm(void)
{
	if (defs_vm != sv.vm) {
		memset(defs, 0, sizeof(defs));
		defs_vm = sv.vm;
	}
}

static int ev_type(float t)
{
	switch ((int)t) {
	case 1: return EVS_STRING;
	case 4: return EVS_ENTITY;
	case 8: case 9: return EVS_INTEGER;
	default: return EVS_FLOAT;
	}
}

static void define(qcvm_t *vm, int src, int ofs)
{
	int num = (int)QC_PFLOAT(vm, 0);
	check_vm();
	if (num < 0 || num >= MAX_CL_STATS) {
		Con_Printf("^3stat %d is out of range\n", num);
		return;
	}
	if (num < 32)
		Con_DPrintf("stat %d is one of the engine's; the game's definition replaces it\n", num);
	defs[num].src = src;
	defs[num].type = ev_type(QC_PFLOAT(vm, 1));
	defs[num].ofs = ofs;
}

static void bi_clientstat(qcvm_t *vm) { define(vm, SRC_FIELD, QC_PINT(vm, 2)); }

static void bi_globalstat(qcvm_t *vm)
{
	int ofs = QC_GlobalOfs(vm, QC_PSTR(vm, 2));
	if (ofs < 0) {
		Con_Printf("^3globalstat: no global %s\n", QC_PSTR(vm, 2));
		return;
	}
	define(vm, SRC_GLOBAL, ofs);
}

static void bi_pointerstat(qcvm_t *vm)
{
	uint32_t addr = (uint32_t)QC_PINT(vm, 2);
	if (addr < vm->globals_addr || (addr - vm->globals_addr) / 4 >= (uint32_t)vm->numglobals) {
		Con_Printf("^3pointerstat: only pointers to globals can be stats\n");
		return;
	}
	define(vm, SRC_GLOBAL, (int)((addr - vm->globals_addr) / 4));
}

const qc_builtin_t sv_stat_builtins[] = {
	{"clientstat", bi_clientstat}, {"addstat", bi_clientstat}, {"globalstat", bi_globalstat},
	{"pointerstat", bi_pointerstat}, {NULL, NULL}};

static float efield(int e, const char *name)
{
	qcdef_t *d = QC_FindField(sv.vm, name);
	return d ? QC_EdictFields(sv.vm, e)[d->ofs].f : 0;
}

static int eint(int e, const char *name)
{
	qcdef_t *d = QC_FindField(sv.vm, name);
	return d ? QC_EdictFields(sv.vm, e)[d->ofs].i : 0;
}

static float gfloat(const char *name)
{
	int ofs = QC_GlobalOfs(sv.vm, name);
	return ofs >= 0 ? sv.vm->globals[ofs].f : 0;
}

/* the value of stat i for client c: type, number bits, string */
static int stat_value(client_t *c, int i, uint32_t *bits, const char **str)
{
	qcvm_t *vm = sv.vm;
	int e = c->edict;
	float f = 0;
	*str = NULL;
	if (defs[i].src) {
		qcval_t *v = defs[i].src == SRC_FIELD ? &QC_EdictFields(vm, e)[defs[i].ofs] : &vm->globals[defs[i].ofs];
		if (defs[i].type == EVS_STRING) {
			*str = QC_GetString(vm, v->i);
			return EVS_STRING;
		}
		*bits = v->u;
		return defs[i].type == EVS_ENTITY ? EVS_INTEGER : defs[i].type;
	}
	switch (i) {
	case STAT_HEALTH: f = efield(e, "health"); break;
	case STAT_WEAPONMODELI: {
		qcdef_t *d = QC_FindField(vm, "weaponmodel");
		const char *m = d ? QC_GetString(vm, QC_EdictFields(vm, e)[d->ofs].i) : "";
		f = m[0] ? (float)SV_ModelIndex(m, false) : 0;
		break;
	}
	case STAT_AMMO: f = efield(e, "currentammo"); break;
	case STAT_ARMOR: f = efield(e, "armorvalue"); break;
	case STAT_WEAPONFRAME: f = efield(e, "weaponframe"); break;
	case STAT_SHELLS: f = efield(e, "ammo_shells"); break;
	case STAT_NAILS: f = efield(e, "ammo_nails"); break;
	case STAT_ROCKETS: f = efield(e, "ammo_rockets"); break;
	case STAT_CELLS: f = efield(e, "ammo_cells"); break;
	case STAT_ACTIVEWEAPON: f = efield(e, "weapon"); break;
	case STAT_TOTALSECRETS: f = gfloat("total_secrets"); break;
	case STAT_TOTALMONSTERS: f = gfloat("total_monsters"); break;
	case STAT_FOUNDSECRETS: f = gfloat("found_secrets"); break;
	case STAT_KILLEDMONSTERS: f = gfloat("killed_monsters"); break;
	case STAT_ITEMS:
		/* items, items2 above bit 23, and the visible serverflags in 28..31 */
		*bits = (uint32_t)efield(e, "items") | ((uint32_t)efield(e, "items2") << 23) |
		        (((uint32_t)gfloat("serverflags") & 15) << 28);
		return EVS_INTEGER;
	case STAT_VIEWHEIGHT: {
		qcdef_t *d = QC_FindField(vm, "view_ofs");
		f = d ? QC_EdictFields(vm, e)[d->ofs + 2].f : 0;
		break;
	}
	case STAT_VIEW2: *bits = (uint32_t)eint(e, "view2"); return EVS_INTEGER;
	case STAT_VIEWZOOM: f = efield(e, "viewzoom") * 255; break;
	default: return 0;
	}
	memcpy(bits, &f, 4);
	return EVS_FLOAT;
}

void SV_ResetStats(client_t *c)
{
	for (int i = 0; i < MAX_CL_STATS; i++) {
		Mem_Free(c->statstr[i]);
		c->statstr[i] = NULL;
	}
	memset(c->stattype, 0, sizeof(c->stattype));
	memset(c->statval, 0, sizeof(c->statval));
}

/* sends what changed since last time */
void SV_UpdateStats(client_t *c)
{
	uint8_t buf[8192];
	msg_t m;
	int count = 0;
	size_t countpos;
	if (!sv.vm || c->bot || c->state != CS_SPAWNED)
		return;
	check_vm();
	MSG_Init(&m, buf, sizeof(buf));
	MSG_WriteByte(&m, svc_stats);
	countpos = m.cursize;
	MSG_WriteVarUInt(&m, 0x3fff); /* patched below: always two bytes */
	m.data[countpos] = 0;
	m.cursize = countpos + 2;
	for (int i = 0; i < MAX_CL_STATS && m.cursize + 300 < sizeof(buf); i++) {
		uint32_t bits = 0;
		const char *s;
		int type = stat_value(c, i, &bits, &s);
		if (!type)
			continue;
		if (type == EVS_STRING) {
			if (c->stattype[i] == EVS_STRING && c->statstr[i] && !strcmp(c->statstr[i], s))
				continue;
			Mem_Free(c->statstr[i]);
			c->statstr[i] = Mem_Strdup(s);
		} else if (c->stattype[i] == type && c->statval[i] == bits) {
			continue;
		}
		c->stattype[i] = (uint8_t)type;
		c->statval[i] = bits;
		MSG_WriteByte(&m, i);
		MSG_WriteByte(&m, type);
		if (type == EVS_STRING)
			MSG_WriteString(&m, s);
		else
			MSG_WriteLong(&m, (int32_t)bits);
		count++;
	}
	if (!count)
		return;
	/* the count as a two byte varuint */
	m.data[countpos] = (uint8_t)((count & 0x7f) | 0x80);
	m.data[countpos + 1] = (uint8_t)(count >> 7);
	SV_ClientReliable(c, m.data, m.cursize);
}
