/* Bulwark engine - player movement as Nuclide's game code runs it, in C, for
 * the DS link's prediction (docs/spec/link.md, "Prediction"). ISC licence,
 * see LICENSE.
 *
 * The PC predicts its player by replaying every command the server hasn't
 * acknowledged through ncPlayer::Physics_Run, which (pm_enginepmove 0) runs
 * PMoveCustom_RunPlayerPhysics: QuakeC, shared by client and server. This is
 * that QC written out in C, function for function, so the DS moves its
 * player as the server will. Each function names the QC it comes from
 * (Nuclide 733a5a1: src/shared/physics/player_pmove.qc, pmove_custom.qc,
 * pmove.qc, src/shared/game/Player.qc, valve/src/shared/Player.qc).
 *
 * Bit for bit where it can be: QC works in float, one operation at a time,
 * left to right, so every expression here is written in the QC's order with
 * float temporaries (no double, no folding, no reordering). The vector
 * builtins are the server's (src/qcvm/qc_builtins_core.c: makevectors is
 * AngleVectors, normalize VectorNormalize, vlen sqrtf of the dot product;
 * qc_exec.c:243 the dot product). fteqcc compiles a unary minus on a float
 * as 0 - x (fteqw qcc_pr_comp.c, the "-" case of the unary parser), which
 * QNEG() keeps, so even the sign of a zero comes out as the server's.
 *
 * QC globals are module state, as in QC: the trace_* results (tr), the
 * v_forward/v_right/v_up that makevectors leaves, the input_* command
 * globals and g_pmoveGroundedBeforeMove. Code reads them exactly where the
 * QC reads them, including stale ones (AccelLadder's trace_ent, Rebound's
 * trace_ent.solid).
 *
 * Left out, being server-only or not movement: footsteps, sounds, damage,
 * touch functions, touchtriggers, portals, vehicles, punch angles, weapon
 * timers, player animation, ProcessInput and the use flagger. Each place
 * says which QC lines it skips.
 *
 * The DS has no FPU: each float operation is a libgcc call of about a
 * microsecond. Where it changes nothing, the work is done on the bits
 * instead (fexact_nds.h: comparisons, sqrtf), or not done again (the vectors
 * of the last angles); every result stays the server's, bit for bit
 * (tools/nds/pmtest -dump checks it against the plain code).
 *
 * Plain C99, no libnds: tools/nds/pmtest.c builds this file on the host. */
#include <math.h>
#include <string.h>

#include "pmove_nds.h"
#include "fexact_nds.h"

/* QC's unary minus on a float: fteqcc emits OP_SUB_F 0, x */
#define QNEG(x) (0.0f - (x))

/* the trace the predicted player moves through, this command's */
static const pmtrace_if *T;
static const pmvars_t *V;
static pmstate_t *S;

static pmhit_t tr;                         /* trace_* */
static float v_forward[3], v_right[3], v_up[3];
static float in_angles[3], in_move[3];     /* input_angles, input_movevalues */
static float in_time;                      /* input_timelength */
static uint32_t in_buttons;                /* input_buttons */
static bool grounded_before;               /* g_pmoveGroundedBeforeMove, pmove_custom.qc:234 */
static uint32_t near_bits;                 /* contents brushes near may give (pmtrace_if.focus) */
static const float vzero[3] = {0, 0, 0};

#define P(k) (V->p[PMV_##k]) /* the player's m_pmoveVars.pm_k: player_pmove.qc */
#define G(k) (V->g[PMV_##k]) /* g_pmoveVars.pm_k: pmove_custom.qc */

/* --- the server's vector builtins ------------------------------------- */

static void vcopy(const float *a, float *out)
{
	out[0] = a[0];
	out[1] = a[1];
	out[2] = a[2];
}

/* QC vector * vector (OP_MUL_V, qc_exec.c:243) and common.h DotProduct */
static float dot(const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* vlen (qc_builtins_core.c bi_vlen); fx_sqrt is sqrtf to the bit. A command asks for the
 * same lengths again (the hull size, a vector it has just normalised), so the last few are
 * kept, keyed on the vector's bits. */
static struct {
	uint32_t key[3];
	float len;
} vlen_memo[4];
static int vlen_next;

static float vlen(const float *v)
{
	uint32_t k0 = fx_bits(v[0]), k1 = fx_bits(v[1]), k2 = fx_bits(v[2]);
	float len;
	for (int i = 0; i < 4; i++)
		if (vlen_memo[i].key[0] == k0 && vlen_memo[i].key[1] == k1 && vlen_memo[i].key[2] == k2)
			return vlen_memo[i].len;
	len = fx_sqrt(dot(v, v));
	vlen_memo[vlen_next].key[0] = k0, vlen_memo[vlen_next].key[1] = k1, vlen_memo[vlen_next].key[2] = k2;
	vlen_memo[vlen_next].len = len;
	vlen_next = (vlen_next + 1) & 3;
	return len;
}

/* normalize (bi_normalize: VectorNormalize, common.c:343-354); its length is vlen's, the
 * same operations on the same bits */
static void normalize(const float *v, float *out)
{
	float len = vlen(v);
	vcopy(v, out);
	if (fx_nonzero(len)) { /* len != 0 */
		float inv = 1.0f / len;
		out[0] *= inv;
		out[1] *= inv;
		out[2] *= inv;
	}
}

/* min() and max() (bi_min, bi_max: BW_MIN/BW_MAX of the first and second) */
static float qmin(float a, float b)
{
	return fx_lt(a, b) ? a : b;
}

static float qmax(float a, float b)
{
	return fx_gt(a, b) ? a : b;
}

/* --- sinf and cosf as the server's C library computes them ------------- */

/* The server runs on x86-64 glibc, whose sinf and cosf (since 2.28; 2.39:
 * sysdeps/ieee754/flt-32/s_sinf.c, s_cosf.c, s_sincosf.h, sincosf_poly.h,
 * s_sincosf_data.c) are Arm's optimized-routines code: math/sinf.c, cosf.c,
 * sincosf.h, sincosf_data.c, Copyright (c) 2018-2024 Arm Limited, MIT
 * licence. That code is ported here, operation for operation in double:
 * the DS's soft double (libgcc) rounds every operation as x86's SSE2 does,
 * so this gives glibc's bits. newlib's sinf/cosf gave another last bit for
 * 13022 of the 65536 yaws a command can carry. glibc's FMA build (picked on
 * CPUs with FMA and AVX2) contracts the multiply-adds; on every angle a
 * command can carry, and on 20 million other arguments, both builds gave
 * the same bits as this (pmtest -selftest checks the angles against the
 * host's own sinf/cosf). */
typedef struct {
	double sign[4];          /* sign of sine in quadrants 0..3 */
	double hpi_inv;          /* 2 / PI * 2^24 (x86 has no TOINT_INTRINSICS) */
	double hpi;              /* PI / 2 */
	double c0, c1, c2, c3, c4; /* cosine polynomial */
	double s1, s2, s3;       /* sine polynomial */
} sincos_t;

/* sincosf_data.c: the cosine polynomial is negated in the 2nd entry */
static const sincos_t sincosf_table[2] = {
	{{1.0, -1.0, -1.0, 1.0}, 0x1.45F306DC9C883p+23, 0x1.921FB54442D18p0, 0x1p0, -0x1.ffffffd0c621cp-2,
	 0x1.55553e1068f19p-5, -0x1.6c087e89a359dp-10, 0x1.99343027bf8c3p-16, -0x1.555545995a603p-3,
	 0x1.1107605230bc4p-7, -0x1.994eb3774cf24p-13},
	{{1.0, -1.0, -1.0, 1.0}, 0x1.45F306DC9C883p+23, 0x1.921FB54442D18p0, -0x1p0, 0x1.ffffffd0c621cp-2,
	 -0x1.55553e1068f19p-5, 0x1.6c087e89a359dp-10, -0x1.99343027bf8c3p-16, -0x1.555545995a603p-3,
	 0x1.1107605230bc4p-7, -0x1.994eb3774cf24p-13},
};

/* 4/PI to 192 bits, 8 new bits an entry (sincosf_data.c) */
static const uint32_t inv_pio4[24] = {
	0xa2, 0xa2f9, 0xa2f983, 0xa2f9836e, 0xf9836e4e, 0x836e4e44, 0x6e4e4415, 0x4e441529,
	0x441529fc, 0x1529fc27, 0x29fc2757, 0xfc2757d1, 0x2757d1f5, 0x57d1f534, 0xd1f534dd, 0xf534ddc0,
	0x34ddc0db, 0xddc0db62, 0xc0db6295, 0xdb629599, 0x6295993c, 0x95993c43, 0x993c4390, 0x3c439041,
};

/* abstop12 (sincosf.h) */
static inline uint32_t abstop12(float x)
{
	return (fx_bits(x) >> 20) & 0x7ff;
}

/* sinf_poly (sincosf.h): the sine polynomial for even n, the cosine one for odd */
static float sinf_poly(double x, double x2, const sincos_t *p, int n)
{
	double x3, x4, x6, x7, s, c, c1, c2, s1;
	if ((n & 1) == 0) {
		x3 = x * x2;
		s1 = p->s2 + x2 * p->s3;
		x7 = x3 * x2;
		s = x + x3 * p->s1;
		return (float)(s + x7 * s1);
	}
	x4 = x2 * x2;
	c2 = p->c3 + x2 * p->c4;
	c1 = p->c0 + x2 * p->c1;
	x6 = x4 * x2;
	c = c1 + x4 * p->c2;
	return (float)(c + x6 * c2);
}

/* reduce_fast (sincosf.h), !TOINT_INTRINSICS */
static double reduce_fast(double x, const sincos_t *p, int *np)
{
	double r = x * p->hpi_inv;
	int n = ((int32_t)r + 0x800000) >> 24;
	*np = n;
	return x - n * p->hpi;
}

/* reduce_large (sincosf.h): |x| >= 120 */
static double reduce_large(uint32_t xi, int *np)
{
	const uint32_t *arr = &inv_pio4[(xi >> 26) & 15];
	int shift = (xi >> 23) & 7;
	uint64_t n, res0, res1, res2;
	xi = (xi & 0xffffff) | 0x800000;
	xi <<= shift;
	res0 = xi * arr[0];
	res1 = (uint64_t)xi * arr[4];
	res2 = (uint64_t)xi * arr[8];
	res0 = (res2 >> 32) | (res0 << 32);
	res0 += res1;
	n = (res0 + (1ULL << 61)) >> 62;
	res0 -= n << 62;
	*np = (int)n;
	return (double)(int64_t)res0 * 0x1.921FB54442D18p-62;
}

/* sinf(y) and cosf(y) (sinf.c, cosf.c), both from one reduction */
static void glibc_sincosf(float y, float *sinp, float *cosp)
{
	double x = y, s;
	int n;
	const sincos_t *p = &sincosf_table[0];
	if (abstop12(y) < abstop12(0x1.921FB6p-1f)) { /* pio4f */
		if (abstop12(y) < abstop12(0x1p-12f)) {
			*sinp = y;
			*cosp = 1.0f;
			return;
		}
		s = x * x;
		*sinp = sinf_poly(x, s, p, 0);
		*cosp = sinf_poly(x, s, p, 1);
	} else if (abstop12(y) < abstop12(120.0f)) {
		x = reduce_fast(x, p, &n);
		s = p->sign[n & 3];
		if (n & 2)
			p = &sincosf_table[1];
		*sinp = sinf_poly(x * s, x * x, p, n);
		*cosp = sinf_poly(x * s, x * x, p, n ^ 1);
	} else if (abstop12(y) < abstop12(INFINITY)) {
		uint32_t xi = fx_bits(y);
		int sign = xi >> 31;
		x = reduce_large(xi, &n);
		s = p->sign[(n + sign) & 3];
		if ((n + sign) & 2)
			p = &sincosf_table[1];
		*sinp = sinf_poly(x * s, x * x, p, n);
		*cosp = sinf_poly(x * s, x * x, p, n ^ 1);
	} else {
		/* __math_invalidf: (y - y) / (y - y), x86's default NaN, or y's own */
		*sinp = *cosp = fx_nan(y) ? fx_float(fx_bits(y) | 0x00400000u) : fx_float(0xffc00000u);
	}
}

/* exported for pmtest -selftest */
void PM_SinCos(float a, float *s, float *c) { glibc_sincosf(a, s, c); }

/* sinf and cosf of the last few angles, by bit pattern, so the same angle
 * is never worked out twice: it returns what they returned for those bits */
static struct {
	uint32_t bits;
	bool valid;
	float s, c;
} trig[4];
static int trig_next;

static void pm_sincos(float a, float *s, float *c)
{
	uint32_t bits = fx_bits(a);
	for (int i = 0; i < 4; i++)
		if (trig[i].valid && trig[i].bits == bits) {
			*s = trig[i].s;
			*c = trig[i].c;
			return;
		}
	glibc_sincosf(a, s, c);
	trig[trig_next].bits = bits;
	trig[trig_next].valid = true;
	trig[trig_next].s = *s;
	trig[trig_next].c = *c;
	trig_next = (trig_next + 1) & 3;
}

/* the vectors of the last few angles, by bit pattern: a command asks for
 * the same two or three again and again */
static struct {
	uint32_t key[3];
	bool valid;
	float f[3], r[3], u[3];
} mvec[4];
static int mvec_next;

/* makevectors (bi_makevectors: AngleVectors, common.c:320-342) */
static void makevectors(const float *angles)
{
	const float d2r = (float)(3.14159265358979323846 / 180.0); /* common.c: (float)(M_PI / 180.0) */
	float sy, cy, sp, cp, sr, cr;
	uint32_t key[3] = {fx_bits(angles[0]), fx_bits(angles[1]), fx_bits(angles[2])};
	for (int i = 0; i < 4; i++)
		if (mvec[i].valid && mvec[i].key[0] == key[0] && mvec[i].key[1] == key[1] && mvec[i].key[2] == key[2]) {
			vcopy(mvec[i].f, v_forward);
			vcopy(mvec[i].r, v_right);
			vcopy(mvec[i].u, v_up);
			return;
		}
	pm_sincos(angles[1] * d2r, &sy, &cy);
	pm_sincos(angles[0] * d2r, &sp, &cp);
	pm_sincos(angles[2] * d2r, &sr, &cr);
	v_forward[0] = cp * cy;
	v_forward[1] = cp * sy;
	v_forward[2] = -sp;
	v_right[0] = -sr * sp * cy + cr * sy;
	v_right[1] = -sr * sp * sy - cr * cy;
	v_right[2] = -sr * cp;
	v_up[0] = cr * sp * cy + sr * sy;
	v_up[1] = cr * sp * sy - sr * cy;
	v_up[2] = cr * cp;
	mvec[mvec_next].key[0] = key[0], mvec[mvec_next].key[1] = key[1], mvec[mvec_next].key[2] = key[2];
	mvec[mvec_next].valid = true;
	vcopy(v_forward, mvec[mvec_next].f);
	vcopy(v_right, mvec[mvec_next].r);
	vcopy(v_up, mvec[mvec_next].u);
	mvec_next = (mvec_next + 1) & 3;
}

/* anglesToForward / anglesToRight (src/common/c_math_vector.h:78-106): the
 * QC's own AngleVectors (:33-58), not the builtin. Its M_PI is 3.14159
 * (fteextensions.qc:874) and fteqcc folds M_PI*2 / 360 in float to
 * 0x1.1df45cp-6, the constant found in coop.dat. The QC caches the last
 * angles; a hit returns the same vectors and only skips the write to
 * v_forward/v_right/v_up, which nothing in the movement reads before the
 * next makevectors (Physics_Run calls these in WaterJump, AirJump and
 * ClampWalkSpeed only), so this one doesn't touch them. */
static void qc_angle_vectors(const float *angles, float *fwd, float *right)
{
	const float k = 0x1.1df45cp-6f;
	float angle, sr, sp, sy, cr, cp, cy;
	angle = angles[1] * k;
	pm_sincos(angle, &sy, &cy);
	angle = angles[0] * k;
	pm_sincos(angle, &sp, &cp);
	angle = angles[2] * k;
	pm_sincos(angle, &sr, &cr);
	if (fwd) {
		fwd[0] = cp * cy;
		fwd[1] = cp * sy;
		fwd[2] = QNEG(sp);
	}
	if (right) {
		right[0] = ((-1.0f * sr) * sp * cy) + ((-1.0f * cr) * QNEG(sy));
		right[1] = ((-1.0f * sr) * sp * sy) + ((-1.0f * cr) * cy);
		right[2] = (-1.0f * sr) * cp;
	}
}

/* fixAngleDelta / fixAngle (src/common/c_math.h:108-133) */
static float fix_angle_delta(float a)
{
	for (;;) {
		if (fx_gt(a, 180))
			a -= 360;
		else if (fx_lt(a, -180))
			a += 360;
		else
			return a;
	}
}

/* lerp (c_math.h:98-100) */
static float lerp(float a, float b, float t)
{
	return (a * (1 - t)) + (b * t);
}

/* --- the world ----------------------------------------------------------- */

/* tracebox(a, mins, maxs, b, MOVE_NORMAL, self) */
static void tracebox(const float *a, const float *mins, const float *maxs, const float *b)
{
	T->move(T->ctx, a, mins, maxs, b, PM_MOVE_NORMAL, 0, &tr);
}

/* PMoveCustom_Contents (pmove_custom.qc:27-35): traceline(org, org,
 * MOVE_EVERYTHING) with hitcontentsmaski -1 */
static uint32_t pmc_contents(const float *org)
{
	T->move(T->ctx, org, vzero, vzero, org, PM_MOVE_EVERYTHING, -1, &tr);
	return tr.contents;
}

/* PMove_IsStuck (pmove.qc:920-930) */
static bool pm_is_stuck(const float *org, const float *mins, const float *maxs)
{
	if (S->solid != PM_SOLID_SLIDEBOX)
		return false;
	tracebox(org, mins, maxs, org);
	return tr.startsolid;
}

/* --- pmove_custom.qc ------------------------------------------------------ */

/* PMoveCustom_Gravity (pmove_custom.qc:38-46) */
static float pmc_gravity(void)
{
	if (fx_nonzero(S->gravity))
		return G(G_GRAVITY) * S->gravity;
	return G(G_GRAVITY);
}

/* PMoveCustom_Categorize (pmove_custom.qc:48-152; LADDERFACING is defined
 * nowhere in valve or coop, so its branch is out) */
static void pmc_categorize(void)
{
	int contents;
	float pos[3], test[3];

	if (S->movetype == PM_MOVETYPE_NOCLIP)
		return;

	pos[0] = S->origin[0] - 0;
	pos[1] = S->origin[1] - 0;
	pos[2] = S->origin[2] - 1;
	tracebox(S->origin, S->mins, S->maxs, pos);

	if (!tr.startsolid) {
		if (fx_lt(tr.fraction, 1.0f) && fx_gt(tr.normal[2], 0.7f)) {
			S->flags |= PM_FL_ONGROUND;
			S->groundent = tr.ent;
		} else {
			S->flags &= ~PM_FL_ONGROUND;
		}
	} else {
		S->groundent = 0;
		S->flags |= PM_FL_ONGROUND;
	}

	S->flags &= ~PM_FL_WATERJUMP;

	if (fx_gt(S->basevel[2], 0))
		S->flags &= ~PM_FL_ONGROUND;

	/* ladder content testing (:83-116). Only a brush entity's skin gives
	 * CONTENTBIT_LADDER: with none near, the probe can't find one and is
	 * left out (the contents probe below overwrites every trace_* it would
	 * have left; makevectors still runs: its vectors are read later) */
	makevectors(S->v_angle);
	if (near_bits & PM_CB_LADDER) {
		for (int k = 0; k < 3; k++)
			pos[k] = S->origin[k] + v_forward[k] * 8.0f;
		T->move(T->ctx, pos, S->mins, S->maxs, pos, PM_MOVE_NORMAL, (int32_t)PM_CB_LADDER, &tr);
	} else {
		tr.contents = 0;
	}
	if (tr.contents & PM_CB_LADDER)
		S->vflags |= PM_VFL_ONLADDER;
	else
		S->vflags &= ~PM_VFL_ONLADDER;

	/* origin + [0, 0, mins[2] + 4] */
	test[0] = S->origin[0] + 0;
	test[1] = S->origin[1] + 0;
	test[2] = S->origin[2] + (S->mins[2] + 4);
	/* only a brush entity's skin gives water, slime or lava (the world's hull
	 * gives solid or nothing, a box body): with none near, the probe can't
	 * find any. Its trace_* would be read next only by AccelLadder (a ladder
	 * near: then it runs); every other path traces again before reading */
	if (near_bits & (PM_CB_WATER | PM_CB_SLIME | PM_CB_LAVA | PM_CB_LADDER))
		contents = (int)pmc_contents(test);
	else
		contents = 0;

	if (contents & PM_CB_WATER)
		contents = PM_CONTENT_WATER;
	else if (contents & PM_CB_SLIME)
		contents = PM_CONTENT_SLIME;
	else if (contents & PM_CB_LAVA)
		contents = PM_CONTENT_LAVA;
	else
		contents = PM_CONTENT_EMPTY;

	/* how far underwater are we? */
	if (contents < PM_CONTENT_SOLID && !(S->vflags & PM_VFL_ONLADDER)) {
		S->watertype = contents;
		for (int k = 0; k < 3; k++)
			test[k] = S->origin[k] + (S->mins[k] + S->maxs[k]) * 0.5f;
		if (pmc_contents(test) & PM_CB_FLUID) {
			for (int k = 0; k < 3; k++)
				test[k] = (S->origin[k] + S->maxs[k]) - (k == 2 ? 1 : 0);
			if (pmc_contents(test) & PM_CB_FLUID)
				S->waterlevel = 3; /* WATERLEVEL_SUBMERGED */
			else
				S->waterlevel = 2; /* WATERLEVEL_CHEST */
		} else {
			S->waterlevel = 1;         /* WATERLEVEL_KNEE */
		}
	} else {
		S->watertype = PM_CONTENT_EMPTY;
		S->waterlevel = 0;                 /* WATERLEVEL_OUTSIDE */
	}
}

/* PMoveCustom_AccelToss (pmove_custom.qc:154-158) */
static void pmc_accel_toss(float move_time)
{
	S->velocity[2] = S->velocity[2] - (pmc_gravity() * move_time);
}

/* PMoveCustom_AccelWater (pmove_custom.qc:160-204): v_forward/right/up are
 * what Categorize's makevectors(v_angle) left (:84) */
static void pmc_accel_water(float move_time)
{
	float friction, wish_speed, wish[3], n[3];

	S->flags &= ~PM_FL_ONGROUND;

	if (in_move[0] == 0 && in_move[1] == 0 && in_move[2] == 0) {
		wish[0] = 0;
		wish[1] = 0;
		wish[2] = -60; /* drift towards bottom */
	} else {
		for (int k = 0; k < 3; k++)
			wish[k] = v_forward[k] * in_move[0];
		for (int k = 0; k < 3; k++)
			wish[k] = wish[k] + v_right[k] * in_move[1];
		for (int k = 0; k < 3; k++)
			wish[k] = wish[k] + v_up[k] * in_move[2];
	}

	wish_speed = vlen(wish);
	if (G(NOSPEEDCAP) == 0 && wish_speed > S->maxspeed)
		wish_speed = S->maxspeed;
	wish_speed = wish_speed * 0.7f;

	/* water friction */
	if (S->velocity[0] != 0 || S->velocity[1] != 0 || S->velocity[2] != 0) {
		friction = vlen(S->velocity) * (1 - move_time * G(FRICTION));
		if (friction > 0) {
			normalize(S->velocity, n);
			for (int k = 0; k < 3; k++)
				S->velocity[k] = n[k] * friction;
		} else {
			vcopy(vzero, S->velocity);
		}
	} else {
		friction = 0;
	}

	/* water acceleration */
	if (wish_speed <= friction)
		return;

	friction = qmin(wish_speed - friction, G(WATERACCELERATE) * wish_speed * move_time);
	normalize(wish, n);
	for (int k = 0; k < 3; k++)
		S->velocity[k] = S->velocity[k] + n[k] * friction;
}

/* PMoveCustom_AccelLadder (pmove_custom.qc:206-230). The jump reads
 * trace_ent's box as the last trace left it: normally the feet probe
 * (:118-119), which hits the world, whose absmin/absmax are 0 0 0. It then
 * feeds a unit vector to makevectors as angles; both kept as written. */
static void pmc_accel_ladder(void)
{
	float pv[3], pos[3], d[3], n[3];

	makevectors(in_angles);
	for (int k = 0; k < 3; k++)
		pv[k] = v_forward[k] * 240;

	if (in_move[0] > 0)
		vcopy(pv, S->velocity);
	else
		vcopy(vzero, S->velocity);

	if (in_buttons & PM_IN_JUMP) {
		for (int k = 0; k < 3; k++)
			pos[k] = tr.entmins[k] + (0.5f * (tr.entmaxs[k] - tr.entmins[k]));
		pos[2] = S->origin[2];
		for (int k = 0; k < 3; k++)
			d[k] = pos[k] - S->origin[k];
		normalize(d, n);
		makevectors(n);
		for (int k = 0; k < 3; k++)
			S->velocity[k] = v_forward[k] * -250;
		for (int k = 0; k < 3; k++)
			S->velocity[k] = S->velocity[k] + v_up[k] * 100;
		S->flags &= ~PM_FL_ONGROUND;
		S->flags &= ~PM_FL_JUMPRELEASED;
	}
}

static void pm_clamp_walk_speed(void);

/* PMoveCustom_AccelFriction (pmove_custom.qc:236-317; the monster branch,
 * :245-255, is SERVER only and never a player) */
static void pmc_accel_friction(float move_time, const float *wish_dir, float wish_speed)
{
	float mult, retain, temp[3], n[3];

	vcopy(S->velocity, temp);
	retain = vlen(temp); /* length(): a cached vlen */

	mult = G(FRICTION);

	/* per frame basis friction modifier */
	if (fx_nonzero(S->friction)) {
		mult /= S->friction;
		S->friction = 0.0f;
	}

	/* apply friction */
	if (fx_gt(retain, 0)) {
		/* if the leading edge is over a dropoff, increase friction (:271-273).
		 * With pm_edgefriction 1 both branches below give the same bits
		 * (x * 1.0f == x) and the next trace (:573) overwrites every trace_*
		 * before anything reads them, so the probe is skipped. */
		bool edge = false;
		if (!fx_eq(G(EDGEFRICTION), 1.0f)) {
			float end[3];
			normalize(temp, n);
			for (int k = 0; k < 3; k++)
				temp[k] = (S->origin[k] + n[k] * 16) + (k == 2 ? 1 : 0) * S->mins[2];
			end[0] = temp[0] + 0;
			end[1] = temp[1] + 0;
			end[2] = temp[2] + -34;
			T->move(T->ctx, temp, vzero, vzero, end, PM_MOVE_NOMONSTERS, 0, &tr);
			edge = fx_eq(tr.fraction, 1.0f);
		}

		if (edge) {
			if (fx_lt(retain, G(STOPSPEED)))
				retain = 1 - move_time * (G(STOPSPEED) / retain) * mult * G(EDGEFRICTION);
			else
				retain = 1 - move_time * mult * G(EDGEFRICTION);
		} else {
			if (fx_lt(retain, G(STOPSPEED)))
				retain = 1 - move_time * (G(STOPSPEED) / retain) * mult;
			else
				retain = 1 - move_time * mult;
		}

		if (fx_lt(retain, 0)) {
			vcopy(vzero, S->velocity);
		} else {
			S->velocity[0] = S->velocity[0] * retain;
			S->velocity[1] = S->velocity[1] * retain;
			/* don't apply friction to horizontal movement... or else jumps get clamped */
			if (S->flags & PM_FL_JUMPRELEASED)
				S->velocity[2] = S->velocity[2] * retain;
		}
	}

	/* acceleration */
	retain = wish_speed - dot(S->velocity, wish_dir);
	if (fx_gt(retain, 0)) {
		float add = qmin(retain, (G(ACCELERATE) * wish_speed) * move_time);
		for (int k = 0; k < 3; k++)
			S->velocity[k] = S->velocity[k] + wish_dir[k] * add;
	}

	if (grounded_before && (S->flags & PM_FL_CLIENT))
		pm_clamp_walk_speed();
}

/* PMoveCustom_AccelNoclip (pmove_custom.qc:319-357) */
static void pmc_accel_noclip(float move_time, const float *wish_dir, float wish_speed)
{
	float apply, retain;
	float *v = S->velocity;

	apply = G(FRICTION);
	if (S->friction != 0.0f) {
		apply /= S->friction;
		S->friction = 0.0f;
	}

	/* apply friction: lengthSquared (c_math_vector.h:205-208) */
	if ((v[0] * v[0]) + (v[1] * v[1]) + (v[2] * v[2])) {
		retain = vlen(v);
		if (retain < G(STOPSPEED))
			retain = 1 - move_time * (G(STOPSPEED) / retain) * apply;
		else
			retain = 1 - move_time * apply;

		if (retain <= 0) {
			vcopy(vzero, v);
		} else {
			for (int k = 0; k < 3; k++)
				v[k] = v[k] * retain;
		}
	}

	/* acceleration */
	retain = wish_speed - dot(v, wish_dir);
	if (retain > 0) {
		float add = qmin(retain, (G(NOCLIPACCELERATE) * wish_speed) * move_time);
		for (int k = 0; k < 3; k++)
			v[k] = v[k] + wish_dir[k] * add;
	}
}

/* PMoveCustom_AccelGravity (pmove_custom.qc:359-378) */
static void pmc_accel_gravity(float move_time, const float *wish_dir, float wish_speed)
{
	float retain;

	S->velocity[2] = S->velocity[2] - (pmc_gravity() * move_time);

	if (fx_lt(wish_speed, 30))
		retain = wish_speed - dot(S->velocity, wish_dir);
	else
		retain = 30 - dot(S->velocity, wish_dir);

	if (fx_gt(retain, 0)) {
		float fric = qmin(retain, (G(AIRACCELERATE) * wish_speed) * move_time);
		for (int k = 0; k < 3; k++)
			S->velocity[k] = S->velocity[k] + wish_dir[k] * fric;
	}
}

/* PMoveCustom_Acceleration (pmove_custom.qc:380-457) */
static void pmc_acceleration(float move_time, bool premove)
{
	float wish[3], wish_dir[3], wish_speed;

	/* the player's own timers (:391-394) */
	if (S->flags & PM_FL_CLIENT) {
		S->time_since_jump -= move_time;
		S->time_since_teleport -= move_time;
	}

	makevectors(in_angles);

	if (premove)
		grounded_before = (S->flags & PM_FL_ONGROUND) ? true : false;

	pmc_categorize();

	if (S->movetype != PM_MOVETYPE_NOCLIP) {
		if (S->movetype == PM_MOVETYPE_TOSS) {
			pmc_accel_toss(move_time);
			return;
		}
		if (S->waterlevel >= 2) {
			pmc_accel_water(move_time);
			return;
		}
	} else {
		for (int k = 0; k < 3; k++)
			wish[k] = v_forward[k] * in_move[0];
		for (int k = 0; k < 3; k++)
			wish[k] = wish[k] + v_right[k] * in_move[1];
		for (int k = 0; k < 3; k++)
			wish[k] = wish[k] + v_up[k] * in_move[2];
		normalize(wish, wish_dir);
		wish_speed = G(NOCLIPSPEED);
		pmc_accel_noclip(move_time, wish_dir, wish_speed);
		return;
	}

	/* on the ground, only yaw matters: makevectors(input_angles[1] * [0,1,0]) */
	if (S->flags & PM_FL_ONGROUND) {
		float yaw[3];
		yaw[0] = in_angles[1] * 0;
		yaw[1] = in_angles[1] * 1;
		yaw[2] = in_angles[1] * 0;
		makevectors(yaw);
	}
	for (int k = 0; k < 3; k++)
		wish[k] = v_forward[k] * in_move[0] + v_right[k] * in_move[1];

	if (S->movetype != PM_MOVETYPE_WALK)
		wish[2] += in_move[2];
	else
		wish[2] = 0;

	normalize(wish, wish_dir);
	wish_speed = vlen(wish);

	if (fx_eq(G(NOSPEEDCAP), 0) && fx_gt(wish_speed, S->maxspeed))
		wish_speed = S->maxspeed;

	if (S->vflags & PM_VFL_ONLADDER)
		pmc_accel_ladder();
	else if (S->flags & PM_FL_ONGROUND)
		pmc_accel_friction(move_time, wish_dir, wish_speed);
	else
		pmc_accel_gravity(move_time, wish_dir, wish_speed);
}

/* PMoveCustom_Rebound (pmove_custom.qc:479-490): trace_ent is whatever the
 * last trace hit, not necessarily the plane's owner */
static void pmc_rebound(const float *normal_in)
{
	float normal[3], d;
	vcopy(normal_in, normal);
	d = dot(S->velocity, normal);
	for (int k = 0; k < 3; k++)
		S->velocity[k] = S->velocity[k] - normal[k] * d;

	if (fx_gt(normal[2], 0.7f)) {
		if (tr.entsolid == PM_SOLID_BSP) {
			S->groundent = tr.ent;
			S->flags |= PM_FL_ONGROUND;
		}
	}
}

/* PMoveCustom_Fix_Origin (pmove_custom.qc:493-548; the #if 0 part stays out) */
static bool pmc_fix_origin(void)
{
	float x, y, z, norg[3], oorg[3];
	vcopy(S->origin, oorg);

	for (z = 0; z < 3; z++) {
		norg[2] = oorg[2] + ((z == 2) ? -1 : z) * 0.0125f;
		for (x = 0; x < 3; x++) {
			norg[0] = oorg[0] + ((x == 2) ? -1 : x) * 0.0125f;
			for (y = 0; y < 3; y++) {
				norg[1] = oorg[1] + ((y == 2) ? -1 : y) * 0.0125f;
				tracebox(norg, S->mins, S->maxs, norg);
				if (!tr.startsolid) {
					vcopy(norg, S->origin);
					return true;
				}
			}
		}
	}
	return false;
}

/* PMoveCustom_Move (pmove_custom.qc:551-682). PMoveCustom_DoTouch (:657,
 * :679) and portals (:648-651, :668-672) are left out: touches are the
 * server's, and the next PSTATE brings what they did (a conveyor's basevel). */
static void pmc_move(void)
{
	float dest[3], saved_plane[3], start[3];
	float stepped, move_time;
	int i;

	/* noclippers don't collide with anything */
	if (S->movetype == PM_MOVETYPE_NOCLIP) {
		for (int k = 0; k < 3; k++)
			S->origin[k] += S->velocity[k] * in_time;
		return;
	}

	/* the QC counts i down in float, 3 2 1, exactly: an int does the same */
	for (i = 3, move_time = in_time; fx_gt(move_time, 0) && i; i--) {
		for (int k = 0; k < 3; k++)
			dest[k] = S->origin[k] + (S->velocity[k] * move_time);
		for (int k = 0; k < 3; k++)
			dest[k] += (S->basevel[k] * move_time);

		tracebox(S->origin, S->mins, S->maxs, dest);

		if (tr.startsolid) {
			if (!pmc_fix_origin())
				return;
			continue;
		}

		/* move us into place */
		vcopy(tr.endpos, S->origin);

		/* no obstacles? no further tests needed */
		if (fx_ge(tr.fraction, 1.0f))
			break;

		/* something is in the way: bounce off it, or step up */
		vcopy(tr.normal, saved_plane);
		move_time -= move_time * tr.fraction;

		if (fx_gt(move_time, 0.0f)) {
			float roof_fraction, roof_plane[3];

			/* step up if we can: the QC writes trace_endpos itself (:597-603) */
			vcopy(S->origin, tr.endpos);
			if (S->flags & PM_FL_ONGROUND)
				tr.endpos[2] += G(STEPSIZE);
			else
				tr.endpos[2] += G(AIRSTEPSIZE);

			vcopy(tr.endpos, dest);
			tracebox(S->origin, S->mins, S->maxs, dest);
			stepped = tr.endpos[2] - S->origin[2];

			roof_fraction = tr.fraction;
			vcopy(tr.normal, roof_plane);

			for (int k = 0; k < 3; k++)
				dest[k] = tr.endpos[k] + (S->velocity[k] * move_time);
			dest[2] = tr.endpos[2]; /* only horizontally */
			for (int k = 0; k < 3; k++)
				dest[k] += (S->basevel[k] * move_time);

			/* clear base-velocity */
			vcopy(vzero, S->basevel);

			/* move forwards */
			vcopy(tr.endpos, start);
			tracebox(start, S->mins, S->maxs, dest);

			/* if we got anywhere, make this raised-step move count */
			if (fx_ge(tr.fraction, 1.0f)) {
				float fwfrac = tr.fraction, fwplane[3];
				vcopy(tr.normal, fwplane);

				/* move down */
				vcopy(tr.endpos, dest);
				dest[2] -= stepped + 1;
				vcopy(tr.endpos, start);
				tracebox(start, S->mins, S->maxs, dest);

				if (fx_lt(tr.fraction, 1.0f) && fx_gt(tr.normal[2], 0.7f)) {
					move_time -= move_time * fwfrac;

					/* bounce off the ceiling */
					if (fx_lt(roof_fraction, 1))
						pmc_rebound(roof_plane);

					if (fx_lt(tr.fraction, 1))
						pmc_rebound(tr.normal);
					else if (fx_lt(fwfrac, 1))
						pmc_rebound(fwplane);
					vcopy(tr.endpos, S->origin);
					continue;
				}
			}
			/* else: a SOLID_PORTAL would transport us (:648-651) */
		}

		/* stepping failed, just bounce off */
		pmc_rebound(saved_plane);
		/* PMoveCustom_DoTouch(trace_ent) (:657): the server's */
	}

	/* touch whatever is below */
	if (S->flags & PM_FL_ONGROUND) {
		vcopy(S->origin, dest);
		dest[2] -= G(STEPSIZE);
		tracebox(S->origin, S->mins, S->maxs, dest);

		if (fx_eq(tr.fraction, 1.0f))
			return;
		/* portal (:668-672) and DoTouch (:679): the server's */
		S->groundent = tr.ent;
	}
}

/* PMoveCustom_RunPlayerPhysics (pmove_custom.qc:692-731); touchtriggers
 * (:727) is the server's */
static void pmc_run(void)
{
	bool flying;

	if (S->movetype == PM_MOVETYPE_NONE)
		return;

	flying = S->movetype == PM_MOVETYPE_NOCLIP || S->movetype == PM_MOVETYPE_FLY;
	if (flying) {
		/* move camera up (noclip, fly) when holding jump, down when crouching */
		if (in_buttons & PM_IN_JUMP)
			in_move[2] = 240;
		if (in_buttons & PM_IN_CROUCH)
			in_move[2] = -240;
	}

	/* half the acceleration before the move and half after */
	pmc_acceleration(in_time / 2, true);
	pmc_move();
	pmc_acceleration(in_time / 2, false);

	vcopy(in_angles, S->angles);
	S->angles[0] *= -0.333f;
}

/* --- player_pmove.qc --------------------------------------------------- */

#define CROUCHING() ((S->vflags & PM_VFL_CROUCHING) != 0) /* IsCrouching */
#define PRONE() ((S->vflags & PM_VFL_PRONE) != 0)         /* IsProne */

/* ncWeapon::PlayerSpeedMod (Weapon.qc:2414-2430) on the state's weapon
 * values (wsm 1, wsmfire 0, wsmzoom 0 with no weapon: the same bits as the
 * QC's speedMod 1.0f) and the state's current vflags */
static float pm_speed_mod(void)
{
	if (fx_gt(S->wsmzoom, 0.0f) && fx_lt(S->viewzoom, 1.0f))
		return S->wsmzoom;
	if (fx_gt(S->wsmfire, 0.0f) && (S->vflags & PM_VFL_FIRING))
		return S->wsmfire;
	return S->wsm;
}

/* ncPlayer::Physics_MaxSpeed (player_pmove.qc:507-548); CanSprint is
 * Player.qc:202-210 */
static float pm_max_speed(void)
{
	float wish = 0.0f;
	float run = P(RUNSPEED);
	float maxstamina = P(STAMINA);
	float crouch = P(CROUCHSPEED);
	float walk = P(WALKSPEED);
	float prone = P(PRONESPEED);
	float mod = pm_speed_mod();
	bool can_sprint = !PRONE() && !CROUCHING() && fx_gt(P(RUNSPEED), 0);

	if (can_sprint && (S->vflags & PM_VFL_SPRINTING)) {
		if (fx_lt(S->stamina, maxstamina)) {
			float thresh = P(STAMINATHRESHOLD);
			if (fx_gt(S->stamina + thresh, maxstamina)) {
				float delta = (S->stamina + thresh) - maxstamina;
				run = lerp(run, walk, delta / thresh);
			}
		}
		wish = fx_ge(S->stamina, maxstamina) ? walk : run;
	} else if (CROUCHING()) {
		wish = crouch;
	} else if (PRONE()) {
		wish = prone;
	} else {
		wish = walk;
	}

	/* moving at another speed than our own, as the server says */
	if (fx_gt(S->speed_target, 0.0f) && fx_gt(walk, 0.0f))
		wish *= S->speed_target / walk;

	return wish * mod;
}

/* ncPlayer::Physics_TopSpeed (player_pmove.qc:550-565) */
static float pm_top_speed(void)
{
	float top = P(WALKSPEED);
	if (fx_gt(S->speed_target, 0.0f) && fx_gt(top, 0.0f))
		top = S->speed_target;
	top *= pm_speed_mod();
	return qmax(S->maxspeed, top);
}

/* ncPlayer::Physics_ClampWalkSpeed (player_pmove.qc:568-612) */
static void pm_clamp_walk_speed(void)
{
	float top = pm_top_speed();
	float cap = P(GROUNDSPEEDCAP) * top;
	float back = P(BACKSPEED);
	float flat[3], flatspeed;

	flat[0] = S->velocity[0];
	flat[1] = S->velocity[1];
	flat[2] = 0;
	flatspeed = vlen(flat);

	/* sliding along walls or turning can't push us past the top speed */
	if (fx_gt(cap, 0.0f) && fx_gt(flatspeed, cap)) {
		float f = cap / flatspeed;
		for (int k = 0; k < 3; k++)
			flat[k] *= f;
		flatspeed = cap;
	}

	/* slower when backing up (Team Fortress 2) */
	if (fx_gt(back, 0.0f) && fx_lt(back, 1.0f) && fx_gt(flatspeed, 100.0f)) {
		float ang[3] = {0, in_angles[1], 0}, fwd[3], right[3], backdot;
		qc_angle_vectors(ang, fwd, NULL);
		qc_angle_vectors(ang, NULL, right);
		backdot = dot(fwd, flat);

		if (fx_lt(backdot, 0.0f)) {
			float backmove[3], sidemove[3], rd, maxback;
			for (int k = 0; k < 3; k++)
				backmove[k] = fwd[k] * backdot;
			rd = dot(right, flat);
			for (int k = 0; k < 3; k++)
				sidemove[k] = right[k] * rd;
			maxback = top * back;

			if (fx_gt(QNEG(backdot), maxback)) {
				float f = maxback / QNEG(backdot);
				for (int k = 0; k < 3; k++)
					backmove[k] *= f;
			}

			for (int k = 0; k < 3; k++)
				flat[k] = backmove[k] + sidemove[k];
			flatspeed = vlen(flat);

			if (fx_gt(flatspeed, top)) {
				float f = top / flatspeed;
				for (int k = 0; k < 3; k++)
					flat[k] *= f;
			}
		}
	}

	S->velocity[0] = flat[0];
	S->velocity[1] = flat[1];
}

/* ncPlayer::Physics_InputPreMove (player_pmove.qc:614-679; vehicles: none,
 * PSTATE stops prediction in one) */
static void pm_input_premove(void)
{
	bool can_move = true;
	float wish, n[3];

	if ((S->vflags & PM_VFL_FROZEN) || S->movetype == PM_MOVETYPE_NONE)
		can_move = false;

	/* freeze in place */
	if (!can_move) {
		vcopy(vzero, in_move);
		in_buttons &= ~(uint32_t)PM_IN_JUMP;
	}

	if (S->vflags & PM_VFL_NOATTACK) {
		in_buttons &= ~(uint32_t)PM_IN_ATTACK;
		in_buttons &= ~(uint32_t)PM_IN_RELOAD;
	}

	/* when pressing the 'use' button, we also walk slower for precision */
	if (in_buttons & PM_IN_USE)
		for (int k = 0; k < 3; k++)
			in_move[k] *= 0.5f;

	/* clamp movement values to max speed */
	wish = vlen(in_move);
	if (fx_gt(wish, S->maxspeed))
		wish = S->maxspeed;
	normalize(in_move, n);
	for (int k = 0; k < 3; k++)
		in_move[k] = n[k] * wish;
}

/* ncPlayer::Physics_WaterJump (player_pmove.qc:402-428) */
static void pm_water_jump(void)
{
	float start[3], end[3], fwd[3];

	start[0] = S->origin[0];
	start[1] = S->origin[1];
	start[2] = S->origin[2];
	start[2] += 8;

	/* look 24 qu ahead for a surface */
	qc_angle_vectors(S->v_angle, fwd, NULL);
	for (int k = 0; k < 3; k++)
		end[k] = start[k] + (fwd[k] * 24);
	T->move(T->ctx, start, vzero, vzero, end, PM_MOVE_NORMAL, 0, &tr);

	if (tr.fraction < 1.0f) {
		/* can we climb up? */
		start[2] += S->maxs[2];
		qc_angle_vectors(S->v_angle, fwd, NULL);
		for (int k = 0; k < 3; k++)
			end[k] = start[k] + (fwd[k] * 24);
		T->move(T->ctx, start, vzero, vzero, end, PM_MOVE_NORMAL, 0, &tr);

		if (tr.fraction == 1.0f) {
			S->velocity[2] = P(WATERJUMPHEIGHT);
			S->flags |= PM_FL_WATERJUMP;
			S->flags &= ~PM_FL_JUMPRELEASED;
		}
	}
}

/* ncPlayer::Physics_WaterMove (player_pmove.qc:431-505): the health test
 * (:438-441) is SERVER only in the QC and kept, the DS mirrors the server;
 * the rest of the SERVER part (damage, air, sounds) is left out */
static void pm_water_move(void)
{
	if (S->movetype == PM_MOVETYPE_NOCLIP)
		return;

	if (fx_lt(S->health, 0))
		return;

	if (!S->waterlevel) {
		if (S->flags & PM_FL_INWATER)
			S->flags &= ~PM_FL_INWATER;
		return;
	}

	if (!(S->flags & PM_FL_INWATER))
		S->flags |= PM_FL_INWATER;

	/* we might need extra velocity to get out of water */
	if (S->waterlevel >= 2)
		pm_water_jump();
}

/* ncPlayer::Physics_SetViewParms (player_pmove.qc:389-400) */
static void pm_set_view_parms(void)
{
	if (CROUCHING())
		vcopy(V->crouchview, S->view_ofs);
	else if (PRONE())
		vcopy(V->proneview, S->view_ofs);
	else
		vcopy(V->standview, S->view_ofs);
}

/* ncPlayer::Physics_Crouch (player_pmove.qc:106-184); CanCrouch is
 * Player.qc:224-227. Standing up moves the origin (SetOrigin, :182). */
static void pm_crouch(void)
{
	bool fix = false, changed = false;
	float org[3], tmins[3] = {0, 0, 0}, tmaxs[3] = {0, 0, 0};

	vcopy(S->origin, org);
	if (S->movetype != PM_MOVETYPE_WALK)
		return;

	if (fx_gt(P(CROUCHHEIGHT), 0) && (in_buttons & PM_IN_CROUCH)) {
		if (!CROUCHING()) {
			vcopy(V->crouchmins, tmins);
			vcopy(V->crouchmaxs, tmaxs);
			if (pm_is_stuck(S->origin, tmins, tmaxs) == false) {
				S->vflags |= PM_VFL_CROUCHING;
				changed = true;
			}
		}
	} else if (CROUCHING()) {
		/* not ducking any more: try to stand up (or go prone) */
		float up[3];
		if (in_buttons & PM_IN_PRONE) {
			vcopy(V->pronemins, tmins);
			vcopy(V->pronemaxs, tmaxs);
		} else {
			vcopy(V->standmins, tmins);
			vcopy(V->standmaxs, tmaxs);
		}

		org[2] = (S->origin[2] + S->mins[2]) - tmins[2];

		/* will we fit here? if not, don't remove the CROUCH flag */
		up[0] = org[0] + 0;
		up[1] = org[1] + 0;
		up[2] = org[2] + 18;
		if (pm_is_stuck(org, tmins, tmaxs) == false) {
			S->vflags &= ~PM_VFL_CROUCHING;
			changed = true;
		} else if (pm_is_stuck(up, tmins, tmaxs) == false) {
			S->vflags &= ~PM_VFL_CROUCHING;
			changed = true;
			fix = true;
		}
	}

	/* state didn't change, don't update size */
	if (!changed)
		return;

	if (CROUCHING()) {
		vcopy(V->crouchmins, S->mins);
		vcopy(V->crouchmaxs, S->maxs);
	} else {
		vcopy(tmins, S->mins);
		vcopy(tmaxs, S->maxs);

		if (fix && pm_is_stuck(org, tmins, tmaxs)) {
			/* check if we can get unstuck by testing up to a few units up */
			for (int i = 0; i < 18; i++) {
				if (pm_is_stuck(org, tmins, tmaxs) == false)
					break;
				org[2] += 1;
			}
		}
		vcopy(org, S->origin); /* SetOrigin(testOrigin) */
	}
}

/* ncPlayer::Physics_Prone (player_pmove.qc:186-266); CanProne is
 * Player.qc:212-216 */
static void pm_prone(void)
{
	bool fix = false, changed = false;
	float org[3], tmins[3] = {0, 0, 0}, tmaxs[3] = {0, 0, 0};

	vcopy(S->origin, org);
	if (S->movetype != PM_MOVETYPE_WALK)
		return;

	if (fx_gt(P(PRONEHEIGHT), 0) && (in_buttons & PM_IN_PRONE)) {
		if (!PRONE()) {
			vcopy(V->pronemins, tmins);
			vcopy(V->pronemaxs, tmaxs);
			if (pm_is_stuck(S->origin, tmins, tmaxs) == false) {
				S->vflags |= PM_VFL_PRONE;
				changed = true;
			}
		}
	} else if (PRONE()) {
		float up[3];
		if (in_buttons & PM_IN_CROUCH) {
			vcopy(V->crouchmins, tmins);
			vcopy(V->crouchmaxs, tmaxs);
		} else {
			vcopy(V->standmins, tmins);
			vcopy(V->standmaxs, tmaxs);
		}

		org[2] = (S->origin[2] + S->mins[2]) - tmins[2];

		up[0] = org[0] + 0;
		up[1] = org[1] + 0;
		up[2] = org[2] + 18;
		if (pm_is_stuck(org, tmins, tmaxs) == false) {
			S->vflags &= ~PM_VFL_PRONE;
			changed = true;
		} else if (pm_is_stuck(up, tmins, tmaxs) == false) {
			S->vflags &= ~PM_VFL_PRONE;
			fix = true;
			changed = true;
		}
	}

	if (!changed)
		return;

	if (PRONE()) {
		vcopy(V->pronemins, S->mins);
		vcopy(V->pronemaxs, S->maxs);
	} else {
		vcopy(tmins, S->mins);
		vcopy(tmaxs, S->maxs);
		vcopy(org, S->origin);

		if (fix && pm_is_stuck(S->origin, S->mins, S->maxs)) {
			for (int i = 1; i < 18; i++) {
				if (pm_is_stuck(S->origin, S->mins, S->maxs) == false)
					break;
				S->origin[2] += 1;
			}
		}
		/* SetOrigin(origin) */
	}
}

/* ncPlayer::Physics_Jump (player_pmove.qc:269-310). The water test (:278)
 * compares WaterLevel(), 2 or 3 there, with CONTENT_WATER and CONTENT_SLIME,
 * so it always takes the lava speed: kept. Sounds (:299-307) are the server's. */
static void pm_jump(void)
{
	if (S->movetype != PM_MOVETYPE_WALK)
		return;

	if (S->waterlevel >= 2) {
		if (S->waterlevel == PM_CONTENT_WATER)
			S->velocity[2] = 100;
		else if (S->waterlevel == PM_CONTENT_SLIME)
			S->velocity[2] = 80;
		else
			S->velocity[2] = 50;
	} else if (S->flags & PM_FL_ONGROUND) {
		/* every jump from the ground starts no faster than a share of the top speed */
		if (P(JUMPSPEEDCAP) > 0.0f) {
			float cap = P(JUMPSPEEDCAP) * pm_top_speed();
			float speed = vlen(S->velocity);
			if (speed > cap) {
				float f = cap / speed;
				for (int k = 0; k < 3; k++)
					S->velocity[k] *= f;
			}
		}
		S->velocity[2] += P(JUMPHEIGHT);
	}
}

/* hlPlayer::Physics_Jump (valve/src/shared/Player.qc:281-300): no
 * MOVETYPE_WALK test, no pm_jumpheight, no pm_jumpspeedcap */
static void pm_jump_hl(void)
{
	if (S->waterlevel >= 2) {
		if (S->watertype == PM_CONTENT_WATER)
			S->velocity[2] = 100;
		else if (S->watertype == PM_CONTENT_SLIME)
			S->velocity[2] = 80;
		else
			S->velocity[2] = 50;
	} else {
		/* Half-Life: Longjump module */
		if (CROUCHING() && (S->rules & PM_R_LONGJUMP)) {
			float ang[3];
			ang[0] = 0;
			ang[1] = S->v_angle[1];
			ang[2] = 0;
			makevectors(ang);
			for (int k = 0; k < 3; k++)
				S->velocity[k] = v_forward[k] * 512;
			S->velocity[2] += 100;
		}
		if (S->flags & PM_FL_ONGROUND)
			S->velocity[2] += 265;
	}
}

/* ncPlayer::Physics_AirJump (player_pmove.qc:314-340) */
static void pm_air_jump(void)
{
	float move[3], ang[3] = {0, in_angles[1], 0}, fwd[3], right[3], dir[3], hspeed;

	vcopy(in_move, move);
	qc_angle_vectors(ang, fwd, NULL);
	qc_angle_vectors(ang, NULL, right);
	for (int k = 0; k < 3; k++)
		dir[k] = (fwd[k] * move[0]) + (right[k] * move[1]);
	hspeed = pm_max_speed();

	dir[2] = 0;

	if (vlen(dir) > 0) {
		float n[3];
		normalize(dir, n);
		S->velocity[0] = n[0] * hspeed;
		S->velocity[1] = n[1] * hspeed;
	} else {
		S->velocity[0] = 0;
		S->velocity[1] = 0;
	}

	S->velocity[2] = P(JUMPHEIGHT);
}

/* ncPlayer::Physics_CheckJump (player_pmove.qc:343-386); autojump is the
 * infokey test of :373, from PSTATE */
static void pm_check_jump(bool premove)
{
	/* unset jump-key whenever it's not set */
	if (!(in_buttons & PM_IN_JUMP)) {
		S->flags |= PM_FL_JUMPRELEASED;
		return;
	}

	if (S->flags & PM_FL_WATERJUMP)
		return;

	if (S->flags & PM_FL_ONGROUND) {
		S->vflags &= ~PM_VFL_AIRJUMPS;
	} else {
		/* air jumps need a fresh press of the jump key */
		if (premove && (S->flags & PM_FL_JUMPRELEASED) && P(AIRJUMPS) > 0) {
			float used = (float)(S->vflags & PM_VFL_AIRJUMPS) / (float)PM_VFL_AIRJUMP_ONE;
			if (used < P(AIRJUMPS) && S->movetype == PM_MOVETYPE_WALK && S->waterlevel < 2) {
				pm_air_jump();
				S->vflags = (S->vflags & ~PM_VFL_AIRJUMPS) |
				            ((uint32_t)(int)((used + 1) * (float)PM_VFL_AIRJUMP_ONE) & PM_VFL_AIRJUMPS);
				S->flags &= ~PM_FL_JUMPRELEASED;
				S->events |= PM_EV_JUMPED;
			}
		}
		return;
	}

	/* if a player wants to be able to hold jump, let them */
	if (!(S->rules & PM_R_AUTOJUMP))
		if (!(S->flags & PM_FL_JUMPRELEASED))
			return;

	if ((in_buttons & PM_IN_JUMP) && premove) {
		if (fx_lt(S->velocity[2], 0))
			S->velocity[2] = 0;

		if (S->rules & PM_R_HL)
			pm_jump_hl();
		else
			pm_jump();
		S->flags &= ~PM_FL_ONGROUND;
		S->flags &= ~PM_FL_JUMPRELEASED;
		S->events |= PM_EV_JUMPED;
	}
}

/* ncPlayer::Physics_Fall (player_pmove.qc:37-104): only what the view
 * needs; punch, sounds and damage are left out */
static void pm_fall(float downforce)
{
	S->landspeed = downforce;
	S->events |= PM_EV_LANDED;
}

/* ncPlayer::Physics_InputSprint (player_pmove.qc:681-701); the button
 * masking of :696-700 changes nothing that moves the player */
static void pm_input_sprint(void)
{
	if (in_buttons & PM_IN_SPRINT) {
		S->stamina += in_time;
	} else {
		float sub = in_time;
		sub *= P(STAMINARATE);
		S->stamina = qmax(0, S->stamina - sub);
	}

	if (fx_gt(S->stamina, P(STAMINA)))
		S->stamina = P(STAMINA);
}

/* hlPlayer::Physics_InputSprint (valve/src/shared/Player.qc:259-279): the
 * flashlight battery */
static void pm_input_sprint_hl(void)
{
	if (S->gflags & PM_GF_FLASHLIGHT) {
		S->stamina += in_time;
		S->vflags |= PM_VFL_SPRINTING;
	} else {
		float sub = in_time;
		sub *= P(STAMINARATE);
		S->stamina = qmax(0, S->stamina - sub);
		S->vflags &= ~PM_VFL_SPRINTING;
	}

	/* turn the flashlight off when empty */
	if (fx_gt(S->stamina, P(STAMINA))) {
		S->stamina = P(STAMINA);
		S->gflags &= ~PM_GF_FLASHLIGHT;
	}
}

/* --- the header's functions ------------------------------------------- */

/* ncPMoveVars::UpdateBoundingBoxes (pmove.qc:207-239) on the player's set */
void PM_SetVars(pmvars_t *v)
{
	const float *p = v->p;
	float half_width = p[PMV_BOXWIDTH] * 0.5f;
	float half_height = p[PMV_NORMALHEIGHT] * 0.5f;

	v->standmins[0] = QNEG(half_width);
	v->standmins[1] = QNEG(half_width);
	v->standmins[2] = 0;
	v->standmaxs[0] = half_width;
	v->standmaxs[1] = half_width;
	v->standmaxs[2] = p[PMV_NORMALHEIGHT];
	v->crouchmins[0] = QNEG(half_width);
	v->crouchmins[1] = QNEG(half_width);
	v->crouchmins[2] = 0;
	v->crouchmaxs[0] = half_width;
	v->crouchmaxs[1] = half_width;
	v->crouchmaxs[2] = p[PMV_CROUCHHEIGHT];
	/* the prone box is pm_normalheight wide, as written (:216-217) */
	v->pronemins[0] = QNEG(half_height);
	v->pronemins[1] = QNEG(half_height);
	v->pronemins[2] = 0;
	v->pronemaxs[0] = half_height;
	v->pronemaxs[1] = half_height;
	v->pronemaxs[2] = p[PMV_PRONEHEIGHT];

	for (int k = 0; k < 3; k++)
		v->standview[k] = v->crouchview[k] = v->proneview[k] = 0;
	v->standview[2] = p[PMV_NORMALVIEWHEIGHT];
	v->crouchview[2] = p[PMV_CROUCHVIEWHEIGHT];
	v->proneview[2] = p[PMV_PRONEVIEWHEIGHT];

	/* pm_boxcenter == true: a float against 1 */
	if (p[PMV_BOXCENTER] == 1.0f) {
		v->standmins[2] -= (p[PMV_NORMALHEIGHT] * 0.5f);
		v->standmaxs[2] -= (p[PMV_NORMALHEIGHT] * 0.5f);
		v->crouchmins[2] -= (p[PMV_CROUCHHEIGHT] * 0.5f);
		v->crouchmaxs[2] -= (p[PMV_CROUCHHEIGHT] * 0.5f);
		v->pronemins[2] -= (p[PMV_PRONEHEIGHT] * 0.5f);
		v->pronemaxs[2] -= (p[PMV_PRONEHEIGHT] * 0.5f);

		v->standview[2] -= (p[PMV_NORMALHEIGHT] * 0.5f);
		v->crouchview[2] -= (p[PMV_CROUCHHEIGHT] * 0.5f);
		v->proneview[2] -= (p[PMV_PRONEHEIGHT] * 0.5f);
	}
}

/* the wire's angles as the server turns them into input_angles
 * (sv_link.c:322-323) */
void PM_CmdAngles(const pmcmd_t *c, float out[3])
{
	int pitch = c->pitch, yaw = c->yaw;
	out[0] = -pitch * (360.0f / 65536.0f);
	out[1] = yaw * (360.0f / 65536.0f);
	out[2] = 0;
}

/* SV_RunClientCommand's v_angle = input_angles (entry.qc:377), then
 * ncPlayer::Physics_Run (player_pmove.qc:750-818) */
void PM_Run(pmstate_t *s, const pmcmd_t *c, const pmvars_t *v, const pmtrace_if *t)
{
	float fallvel, basevel, saved_move[3];
	uint32_t saved_buttons;
	bool was_on_ground, sprint_fail;

	S = s;
	V = v;
	T = t;

	/* the input_* globals of this command */
	in_time = (float)c->msec / 1000.0f;
	PM_CmdAngles(c, in_angles);
	for (int k = 0; k < 3; k++)
		in_move[k] = (float)c->move[k];
	in_buttons = c->buttons;
	vcopy(in_angles, s->v_angle);
	s->landspeed = 0;
	s->events = 0;

	/* the broadphase: everything this command can reach (§3.2 of the spec) */
	near_bits = 0xffffffffu;
	if (t->focus) {
		float r = (vlen(s->velocity) + vlen(s->basevel) + 1000) * (float)c->msec / 1000.0f;
		r += P(STEPSIZE) + 64 + qmax(vlen(s->mins), vlen(s->maxs));
		near_bits = t->focus(t->ctx, s->origin, s->mins, s->maxs, r, 0);
	}

	fallvel = (s->flags & PM_FL_ONGROUND) ? 0 : QNEG(s->velocity[2]);
	basevel = s->basevel[2];
	was_on_ground = (s->flags & PM_FL_ONGROUND) ? true : false;

	vcopy(in_move, saved_move);
	saved_buttons = in_buttons;

	sprint_fail = ((in_buttons & PM_IN_CROUCH) || (in_buttons & PM_IN_PRONE)) ? true : false;
	if ((in_buttons & PM_IN_SPRINT) && sprint_fail == false)
		s->vflags |= PM_VFL_SPRINTING;
	else
		s->vflags &= ~PM_VFL_SPRINTING;

	s->maxspeed = pm_max_speed();

	pm_input_premove();
	/* Footsteps_Update (:781): sounds, the server's */
	pm_water_move();

	/* grappling hook (:787-790) */
	if (fx_nonzero(s->hookvel[0]) || fx_nonzero(s->hookvel[1]) || fx_nonzero(s->hookvel[2])) {
		float f;
		for (int k = 0; k < 3; k++)
			s->velocity[k] = (s->hookvel[k] - s->origin[k]);
		f = 1 / (vlen(s->velocity) / 750);
		for (int k = 0; k < 3; k++)
			s->velocity[k] = (s->velocity[k] * f);
	}

	pm_set_view_parms();
	pm_crouch();
	pm_prone();
	pm_check_jump(true);

	pmc_run();

	pm_check_jump(false);

	if (was_on_ground == false && (s->flags & PM_FL_ONGROUND)) {
		if (s->waterlevel != 0)
			fallvel = 0;
		pm_fall(fallvel - basevel);
	}

	vcopy(saved_move, in_move);
	in_buttons = saved_buttons;

	/* Physics_InputPostMove (:704-747): of it only the sprint call moves
	 * the player; timers, punch, animation and ProcessInput are left out */
	if (s->rules & PM_R_HL)
		pm_input_sprint_hl();
	else
		pm_input_sprint();

	for (int k = 0; k < 3; k++)
		s->angles[k] = fix_angle_delta(s->angles[k]);
	/* the use flagger (:820-835) is the server's */
}
