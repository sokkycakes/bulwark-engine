/* Bulwark engine - builtins shared by every QC module (math, strings,
 * cvars, entities, memory). Behaviour follows the builtin descriptions that
 * ship with the QC headers; see docs/spec/builtins.md.
 * ISC licence, see LICENSE. */
#include "qc_internal.h"
#include "../common/cvar.h"
#include "../common/cmd.h"
#include "../common/digest.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define D2R (float)(M_PI / 180.0)

/* --- helpers --------------------------------------------------------- */

static void concat_args(qcvm_t *vm, int first, strbuf_t *sb)
{
	for (int i = first; i < vm->argc; i++)
		SB_AppendStr(sb, QC_PSTR(vm, i));
	if (!sb->data)
		SB_Append(sb, "", 0);
}

static void return_concat(qcvm_t *vm, int first)
{
	strbuf_t sb = {0};
	concat_args(vm, first, &sb);
	QC_ReturnString(vm, sb.data);
	SB_Free(&sb);
}

static void format_float(char *out, size_t size, float f)
{
	if (f == (float)(int)f && fabsf(f) < 1e9f)
		snprintf(out, size, "%d", (int)f);
	else
		snprintf(out, size, "%g", f);
}

/* --- math ------------------------------------------------------------ */

static void bi_makevectors(qcvm_t *vm)
{
	float f[3], r[3], u[3];
	AngleVectors(QC_PVEC(vm, 0), f, r, u);
	QC_SetGVec(vm, vm->gofs.v_forward, f);
	QC_SetGVec(vm, vm->gofs.v_right, r);
	QC_SetGVec(vm, vm->gofs.v_up, u);
}

static void bi_normalize(qcvm_t *vm)
{
	float v[3];
	VectorCopy(QC_PVEC(vm, 0), v);
	VectorNormalize(v);
	QC_ReturnVector(vm, v);
}

static void bi_vlen(qcvm_t *vm)
{
	float *v = QC_PVEC(vm, 0);
	QC_ReturnFloat(vm, sqrtf(DotProduct(v, v)));
}

static float vec_yaw(const float *v)
{
	float yaw;
	if (v[0] == 0 && v[1] == 0)
		return 0;
	yaw = atan2f(v[1], v[0]) * (180.0f / (float)M_PI);
	if (yaw < 0)
		yaw += 360;
	return yaw;
}

static void bi_vectoyaw(qcvm_t *vm) { QC_ReturnFloat(vm, vec_yaw(QC_PVEC(vm, 0))); }

static void vec_angles(const float *fwd, const float *up, float *out)
{
	float yaw, pitch, roll = 0;
	float forward2d = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1]);
	if (forward2d == 0) {
		yaw = 0;
		pitch = fwd[2] > 0 ? 90 : 270;
	} else {
		yaw = vec_yaw(fwd);
		pitch = atan2f(fwd[2], forward2d) * (180.0f / (float)M_PI);
		if (pitch < 0)
			pitch += 360;
	}
	if (up) {
		/* roll is the angle between the requested up and the up implied by yaw+pitch */
		float ang[3] = {-pitch, yaw, 0}, r[3], u[3];
		AngleVectors(ang, NULL, r, u);
		roll = atan2f(-DotProduct(up, r), DotProduct(up, u)) * (180.0f / (float)M_PI);
	}
	out[0] = pitch;
	out[1] = yaw;
	out[2] = roll;
}

static void bi_vectoangles(qcvm_t *vm)
{
	float out[3];
	vec_angles(QC_PVEC(vm, 0), vm->argc >= 2 ? QC_PVEC(vm, 1) : NULL, out);
	QC_ReturnVector(vm, out);
}

static void bi_random(qcvm_t *vm)
{
	float r = QC_Random(vm);
	if (vm->argc >= 2)
		r = QC_PFLOAT(vm, 0) + r * (QC_PFLOAT(vm, 1) - QC_PFLOAT(vm, 0));
	else if (vm->argc == 1)
		r *= QC_PFLOAT(vm, 0);
	QC_ReturnFloat(vm, r);
}

static void bi_randomvec(qcvm_t *vm)
{
	float v[3];
	do {
		v[0] = QC_Random(vm) * 2 - 1;
		v[1] = QC_Random(vm) * 2 - 1;
		v[2] = QC_Random(vm) * 2 - 1;
	} while (DotProduct(v, v) > 1);
	QC_ReturnVector(vm, v);
}

static void bi_rint(qcvm_t *vm)
{
	float f = QC_PFLOAT(vm, 0);
	QC_ReturnFloat(vm, f >= 0 ? floorf(f + 0.5f) : -floorf(-f + 0.5f));
}
static void bi_floor(qcvm_t *vm) { QC_ReturnFloat(vm, floorf(QC_PFLOAT(vm, 0))); }
static void bi_ceil(qcvm_t *vm) { QC_ReturnFloat(vm, ceilf(QC_PFLOAT(vm, 0))); }
static void bi_fabs(qcvm_t *vm) { QC_ReturnFloat(vm, fabsf(QC_PFLOAT(vm, 0))); }
static void bi_sin(qcvm_t *vm) { QC_ReturnFloat(vm, sinf(QC_PFLOAT(vm, 0))); }
static void bi_cos(qcvm_t *vm) { QC_ReturnFloat(vm, cosf(QC_PFLOAT(vm, 0))); }
static void bi_tan(qcvm_t *vm) { QC_ReturnFloat(vm, tanf(QC_PFLOAT(vm, 0))); }
static void bi_asin(qcvm_t *vm) { QC_ReturnFloat(vm, asinf(QC_PFLOAT(vm, 0))); }
static void bi_acos(qcvm_t *vm) { QC_ReturnFloat(vm, acosf(QC_PFLOAT(vm, 0))); }
static void bi_atan(qcvm_t *vm) { QC_ReturnFloat(vm, atanf(QC_PFLOAT(vm, 0))); }
static void bi_atan2(qcvm_t *vm) { QC_ReturnFloat(vm, atan2f(QC_PFLOAT(vm, 0), QC_PFLOAT(vm, 1))); }
static void bi_sqrt(qcvm_t *vm) { QC_ReturnFloat(vm, sqrtf(QC_PFLOAT(vm, 0))); }
static void bi_pow(qcvm_t *vm) { QC_ReturnFloat(vm, powf(QC_PFLOAT(vm, 0), QC_PFLOAT(vm, 1))); }

static void bi_logarithm(qcvm_t *vm)
{
	float v = logf(QC_PFLOAT(vm, 0));
	if (vm->argc >= 2 && QC_PFLOAT(vm, 1) > 0)
		v /= logf(QC_PFLOAT(vm, 1));
	QC_ReturnFloat(vm, v);
}

static void bi_min(qcvm_t *vm)
{
	float m = QC_PFLOAT(vm, 0);
	for (int i = 1; i < vm->argc; i++)
		m = BW_MIN(m, QC_PFLOAT(vm, i));
	QC_ReturnFloat(vm, m);
}

static void bi_max(qcvm_t *vm)
{
	float m = QC_PFLOAT(vm, 0);
	for (int i = 1; i < vm->argc; i++)
		m = BW_MAX(m, QC_PFLOAT(vm, i));
	QC_ReturnFloat(vm, m);
}

static void bi_bound(qcvm_t *vm)
{
	float lo = QC_PFLOAT(vm, 0), v = QC_PFLOAT(vm, 1), hi = QC_PFLOAT(vm, 2);
	if (v < lo)
		v = lo;
	if (v > hi)
		v = hi;
	QC_ReturnFloat(vm, v);
}

static void bi_mod(qcvm_t *vm)
{
	float b = QC_PFLOAT(vm, 1);
	QC_ReturnFloat(vm, b ? fmodf(QC_PFLOAT(vm, 0), b) : 0);
}

static void bi_anglemod(qcvm_t *vm)
{
	float a = fmodf(QC_PFLOAT(vm, 0), 360);
	if (a < 0)
		a += 360;
	QC_ReturnFloat(vm, a);
}

static void bi_bitshift(qcvm_t *vm)
{
	int32_t v = (int32_t)QC_PFLOAT(vm, 0);
	int s = (int)QC_PFLOAT(vm, 1);
	if (s >= 0)
		v = s > 31 ? 0 : (int32_t)((uint32_t)v << s);
	else
		v = -s > 31 ? 0 : v >> -s;
	QC_ReturnFloat(vm, (float)v);
}

static void bi_crossproduct(qcvm_t *vm)
{
	float *a = QC_PVEC(vm, 0), *b = QC_PVEC(vm, 1), c[3];
	c[0] = a[1] * b[2] - a[2] * b[1];
	c[1] = a[2] * b[0] - a[0] * b[2];
	c[2] = a[0] * b[1] - a[1] * b[0];
	QC_ReturnVector(vm, c);
}

static void get_axis(qcvm_t *vm, float *f, float *r, float *u)
{
	for (int i = 0; i < 3; i++) {
		f[i] = QC_GFloat(vm, vm->gofs.v_forward + i);
		r[i] = QC_GFloat(vm, vm->gofs.v_right + i);
		u[i] = QC_GFloat(vm, vm->gofs.v_up + i);
	}
}

/* Rotates the current v_forward/v_right/v_up basis by another basis given in
 * that basis' own local frame (forward, left, up). Unverified against the
 * reference engine; see docs/spec/builtins.md. */
static void rotate_axis(qcvm_t *vm, const float *bf, const float *br, const float *bu)
{
	float f[3], r[3], u[3], nf[3], nr[3], nu[3];
	float bl[3] = {-br[0], -br[1], -br[2]};
	get_axis(vm, f, r, u);
	for (int i = 0; i < 3; i++) {
		float left = -r[i];
		nf[i] = bf[0] * f[i] + bf[1] * left + bf[2] * u[i];
		nr[i] = -(bl[0] * f[i] + bl[1] * left + bl[2] * u[i]);
		nu[i] = bu[0] * f[i] + bu[1] * left + bu[2] * u[i];
	}
	QC_SetGVec(vm, vm->gofs.v_forward, nf);
	QC_SetGVec(vm, vm->gofs.v_right, nr);
	QC_SetGVec(vm, vm->gofs.v_up, nu);
}

static void bi_rotatevectorsbyangle(qcvm_t *vm)
{
	float ang[3], f[3], r[3], u[3];
	VectorCopy(QC_PVEC(vm, 0), ang);
	AngleVectors(ang, f, r, u);
	rotate_axis(vm, f, r, u);
}

static void bi_rotatevectorsbyvectors(qcvm_t *vm)
{
	rotate_axis(vm, QC_PVEC(vm, 0), QC_PVEC(vm, 1), QC_PVEC(vm, 2));
}

static void bi_vectorvectors(qcvm_t *vm)
{
	float f[3], r[3], u[3], tmp[3] = {0, 0, 1};
	VectorCopy(QC_PVEC(vm, 0), f);
	VectorNormalize(f);
	if (fabsf(f[2]) > 0.999f) {
		tmp[0] = 1;
		tmp[2] = 0;
	}
	/* right = forward x up_hint, up = right x forward */
	r[0] = f[1] * tmp[2] - f[2] * tmp[1];
	r[1] = f[2] * tmp[0] - f[0] * tmp[2];
	r[2] = f[0] * tmp[1] - f[1] * tmp[0];
	VectorNormalize(r);
	u[0] = r[1] * f[2] - r[2] * f[1];
	u[1] = r[2] * f[0] - r[0] * f[2];
	u[2] = r[0] * f[1] - r[1] * f[0];
	QC_SetGVec(vm, vm->gofs.v_forward, f);
	QC_SetGVec(vm, vm->gofs.v_right, r);
	QC_SetGVec(vm, vm->gofs.v_up, u);
}

static void bi_ftoi(qcvm_t *vm) { QC_ReturnInt(vm, (int32_t)QC_PFLOAT(vm, 0)); }

static void bi_itof(qcvm_t *vm)
{
	int32_t v = QC_PINT(vm, 0);
	if (vm->argc >= 2) {
		int shift = (int)QC_PFLOAT(vm, 1);
		int bits = vm->argc >= 3 ? (int)QC_PFLOAT(vm, 2) : 24;
		uint32_t mask = bits >= 32 ? 0xffffffffu : ((1u << bits) - 1);
		v = (int32_t)(((uint32_t)v >> (shift & 31)) & mask);
	}
	QC_ReturnFloat(vm, (float)v);
}

/* --- strings --------------------------------------------------------- */

static void bi_ftos(qcvm_t *vm)
{
	char buf[64];
	format_float(buf, sizeof(buf), QC_PFLOAT(vm, 0));
	QC_ReturnString(vm, buf);
}

static void bi_vtos(qcvm_t *vm)
{
	char buf[160], x[48], y[48], z[48];
	float *v = QC_PVEC(vm, 0);
	format_float(x, sizeof(x), v[0]);
	format_float(y, sizeof(y), v[1]);
	format_float(z, sizeof(z), v[2]);
	snprintf(buf, sizeof(buf), "'%s %s %s'", x, y, z);
	QC_ReturnString(vm, buf);
}

static void bi_etos(qcvm_t *vm)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "entity %d", QC_PENT(vm, 0));
	QC_ReturnString(vm, buf);
}

static void bi_stof(qcvm_t *vm) { QC_ReturnFloat(vm, (float)atof(QC_PSTR(vm, 0))); }

static void parse_vector(const char *s, float *v)
{
	char *end;
	v[0] = v[1] = v[2] = 0;
	while (*s == ' ' || *s == '\t' || *s == '\'')
		s++;
	for (int i = 0; i < 3; i++) {
		v[i] = strtof(s, &end);
		if (end == s)
			break;
		s = end;
		while (*s == ' ' || *s == '\t')
			s++;
	}
}

static void bi_stov(qcvm_t *vm)
{
	float v[3];
	parse_vector(QC_PSTR(vm, 0), v);
	QC_ReturnVector(vm, v);
}

static void bi_stoi(qcvm_t *vm) { QC_ReturnInt(vm, (int32_t)strtol(QC_PSTR(vm, 0), NULL, 0)); }
static void bi_stoh(qcvm_t *vm) { QC_ReturnInt(vm, (int32_t)strtoul(QC_PSTR(vm, 0), NULL, 16)); }

static void bi_itos(qcvm_t *vm)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", QC_PINT(vm, 0));
	QC_ReturnString(vm, buf);
}

static void bi_htos(qcvm_t *vm)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "%08x", (unsigned)QC_PINT(vm, 0));
	QC_ReturnString(vm, buf);
}

static void bi_strlen(qcvm_t *vm) { QC_ReturnFloat(vm, (float)strlen(QC_PSTR(vm, 0))); }
static void bi_memstrsize(qcvm_t *vm) { QC_ReturnFloat(vm, (float)strlen(QC_PSTR(vm, 0))); }
static void bi_strcat(qcvm_t *vm) { return_concat(vm, 0); }

static void bi_digest_hex(qcvm_t *vm)
{
	strbuf_t sb = {0};
	char hex[65];
	concat_args(vm, 1, &sb);
	if (Digest_Hex(QC_PSTR(vm, 0), sb.data, sb.len, hex, sizeof(hex)))
		QC_ReturnString(vm, hex);
	else
		QC_ReturnInt(vm, 0);
	SB_Free(&sb);
}

static void bi_crc16(qcvm_t *vm)
{
	strbuf_t sb = {0};
	concat_args(vm, 1, &sb);
	if (QC_PFLOAT(vm, 0))
		for (size_t i = 0; i < sb.len; i++)
			sb.data[i] = (char)tolower((unsigned char)sb.data[i]);
	QC_ReturnFloat(vm, Digest_CRC16(sb.data, sb.len));
	SB_Free(&sb);
}
static void bi_strunzone(qcvm_t *vm) { (void)vm; }

static void bi_substring(qcvm_t *vm)
{
	const char *s = QC_PSTR(vm, 0);
	int len = (int)strlen(s);
	int start = (int)QC_PFLOAT(vm, 1);
	int n = (int)QC_PFLOAT(vm, 2);
	if (start < 0)
		start += len;
	if (start < 0)
		start = 0;
	if (start > len)
		start = len;
	if (n < 0)
		n = len - start + n + 1; /* relative to the terminator */
	if (n < 0)
		n = 0;
	if (start + n > len)
		n = len - start;
	vm->globals[OFS_RETURN].i = QC_NewStringLen(vm, s + start, (size_t)n);
}

static void bi_strstrofs(qcvm_t *vm)
{
	const char *s = QC_PSTR(vm, 0), *sub = QC_PSTR(vm, 1), *hit;
	int start = vm->argc >= 3 ? (int)QC_PFLOAT(vm, 2) : 0;
	int len = (int)strlen(s);
	if (start < 0 || start > len) {
		QC_ReturnFloat(vm, -1);
		return;
	}
	hit = strstr(s + start, sub);
	QC_ReturnFloat(vm, hit ? (float)(hit - s) : -1);
}

static void bi_str2chr(qcvm_t *vm)
{
	const char *s = QC_PSTR(vm, 0);
	int len = (int)strlen(s), i = (int)QC_PFLOAT(vm, 1);
	if (i < 0)
		i += len;
	QC_ReturnFloat(vm, (i >= 0 && i < len) ? (float)(unsigned char)s[i] : 0);
}

static void bi_chr2str(qcvm_t *vm)
{
	char buf[QC_MAX_EXTPARMS + MAX_PARMS + 1];
	int n = 0;
	for (int i = 0; i < vm->argc && n < (int)sizeof(buf) - 1; i++) {
		int ch = (int)QC_PFLOAT(vm, i);
		if (ch)
			buf[n++] = (char)ch;
	}
	vm->globals[OFS_RETURN].i = QC_NewStringLen(vm, buf, (size_t)n);
}

static void case_convert(qcvm_t *vm, int (*conv)(int))
{
	char *s = Mem_Strdup(QC_PSTR(vm, 0));
	for (char *p = s; *p; p++)
		*p = (char)conv((unsigned char)*p);
	QC_ReturnString(vm, s);
	Mem_Free(s);
}
static void bi_strtolower(qcvm_t *vm) { case_convert(vm, tolower); }
static void bi_strtoupper(qcvm_t *vm) { case_convert(vm, toupper); }

static void str_replace(qcvm_t *vm, bool nocase)
{
	const char *search = QC_PSTR(vm, 0), *repl = QC_PSTR(vm, 1), *subj = QC_PSTR(vm, 2);
	size_t slen = strlen(search);
	strbuf_t sb = {0};
	if (!slen) {
		QC_ReturnString(vm, subj);
		return;
	}
	while (*subj) {
		if (nocase ? !Str_NICmp(subj, search, slen) : !strncmp(subj, search, slen)) {
			SB_AppendStr(&sb, repl);
			subj += slen;
		} else {
			SB_Append(&sb, subj, 1);
			subj++;
		}
	}
	QC_ReturnString(vm, sb.data ? sb.data : "");
	SB_Free(&sb);
}
static void bi_strreplace(qcvm_t *vm) { str_replace(vm, false); }
static void bi_strireplace(qcvm_t *vm) { str_replace(vm, true); }

static const char *str_ofs(const char *s, int ofs)
{
	int len = (int)strlen(s);
	return s + (ofs < 0 ? 0 : ofs > len ? len : ofs);
}

static void bi_strncmp(qcvm_t *vm)
{
	const char *a = QC_PSTR(vm, 0), *b = QC_PSTR(vm, 1);
	if (vm->argc < 3) {
		QC_ReturnFloat(vm, (float)strcmp(a, b));
		return;
	}
	if (vm->argc >= 4)
		a = str_ofs(a, (int)QC_PFLOAT(vm, 3));
	if (vm->argc >= 5)
		b = str_ofs(b, (int)QC_PFLOAT(vm, 4));
	QC_ReturnFloat(vm, (float)strncmp(a, b, (size_t)BW_MAX(0, (int)QC_PFLOAT(vm, 2))));
}

static void bi_strcasecmp(qcvm_t *vm) { QC_ReturnFloat(vm, (float)Str_ICmp(QC_PSTR(vm, 0), QC_PSTR(vm, 1))); }

static void bi_strncasecmp(qcvm_t *vm)
{
	const char *a = QC_PSTR(vm, 0), *b = QC_PSTR(vm, 1);
	if (vm->argc >= 4)
		a = str_ofs(a, (int)QC_PFLOAT(vm, 3));
	if (vm->argc >= 5)
		b = str_ofs(b, (int)QC_PFLOAT(vm, 4));
	QC_ReturnFloat(vm, (float)Str_NICmp(a, b, (size_t)BW_MAX(0, (int)QC_PFLOAT(vm, 2))));
}

static void bi_strtrim(qcvm_t *vm)
{
	const char *s = QC_PSTR(vm, 0), *e;
	while (*s && isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		e--;
	vm->globals[OFS_RETURN].i = QC_NewStringLen(vm, s, (size_t)(e - s));
}

static void bi_strpad(qcvm_t *vm)
{
	int pad = (int)QC_PFLOAT(vm, 0);
	strbuf_t sb = {0}, out = {0};
	int len;
	concat_args(vm, 1, &sb);
	len = (int)sb.len;
	if (pad < 0) {
		for (int i = len; i < -pad; i++)
			SB_Append(&out, " ", 1);
		SB_Append(&out, sb.data, sb.len);
	} else {
		SB_Append(&out, sb.data, sb.len);
		for (int i = len; i < pad; i++)
			SB_Append(&out, " ", 1);
	}
	QC_ReturnString(vm, out.data);
	SB_Free(&sb);
	SB_Free(&out);
}

/* skips a ^ colour/markup code at p, returning its length or 0 */
static int markup_len(const char *p)
{
	if (p[0] != '^')
		return 0;
	if (isdigit((unsigned char)p[1]))
		return 2;
	if (p[1] == 'x' && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3]) && isxdigit((unsigned char)p[4]))
		return 5;
	if (p[1] == '^')
		return 1;
	return 0;
}

static void bi_strdecolorize(qcvm_t *vm)
{
	const char *s = QC_PSTR(vm, 0);
	strbuf_t sb = {0};
	while (*s) {
		int m = markup_len(s);
		if (m) {
			s += m;
			continue;
		}
		SB_Append(&sb, s, 1);
		s++;
	}
	QC_ReturnString(vm, sb.data ? sb.data : "");
	SB_Free(&sb);
}

static void bi_strlennocol(qcvm_t *vm)
{
	const char *s = QC_PSTR(vm, 0);
	int n = 0;
	while (*s) {
		int m = markup_len(s);
		if (m) {
			s += m;
			continue;
		}
		n++;
		s++;
	}
	QC_ReturnFloat(vm, (float)n);
}

static void bi_strconv(qcvm_t *vm)
{
	int ccase = (int)QC_PFLOAT(vm, 0);
	strbuf_t sb = {0};
	concat_args(vm, 3, &sb);
	for (size_t i = 0; i < sb.len; i++) {
		unsigned char ch = (unsigned char)sb.data[i];
		if (ccase == 1)
			ch = (unsigned char)tolower(ch);
		else if (ccase == 2)
			ch = (unsigned char)toupper(ch);
		sb.data[i] = (char)ch;
	}
	/* TODO: the red/gold Quake charset remapping (redalpha, redchars) */
	QC_ReturnString(vm, sb.data);
	SB_Free(&sb);
}

/* infostrings: \key\value\key\value */
static bool info_find(const char *info, const char *key, const char **vstart, size_t *vlen, const char **kstart)
{
	const char *p = info;
	size_t klen = strlen(key);
	while (*p == '\\') {
		const char *k = p + 1, *ke = strchr(k, '\\'), *v, *ve;
		if (!ke)
			return false;
		v = ke + 1;
		ve = strchr(v, '\\');
		if (!ve)
			ve = v + strlen(v);
		if ((size_t)(ke - k) == klen && !strncmp(k, key, klen)) {
			*vstart = v;
			*vlen = (size_t)(ve - v);
			if (kstart)
				*kstart = p;
			return true;
		}
		p = ve;
	}
	return false;
}

static void bi_infoget(qcvm_t *vm)
{
	const char *v;
	size_t len;
	if (info_find(QC_PSTR(vm, 0), QC_PSTR(vm, 1), &v, &len, NULL))
		vm->globals[OFS_RETURN].i = QC_NewStringLen(vm, v, len);
	else
		vm->globals[OFS_RETURN].i = 0;
}

static void bi_infoadd(qcvm_t *vm)
{
	const char *info = QC_PSTR(vm, 0), *key = QC_PSTR(vm, 1), *val = QC_PSTR(vm, 2);
	const char *v, *k;
	size_t len;
	strbuf_t sb = {0};
	if (strchr(key, '\\') || strchr(val, '\\')) {
		QC_ReturnString(vm, info);
		return;
	}
	if (info_find(info, key, &v, &len, &k)) {
		SB_Append(&sb, info, (size_t)(k - info));
		SB_AppendStr(&sb, v + len);
	} else {
		SB_AppendStr(&sb, info);
	}
	if (*val)
		SB_Printf(&sb, "\\%s\\%s", key, val);
	QC_ReturnString(vm, sb.data ? sb.data : "");
	SB_Free(&sb);
}

static void bi_strftime(qcvm_t *vm)
{
	char out[1024];
	time_t t = time(NULL);
	struct tm tmv;
	strbuf_t fmt = {0};
	concat_args(vm, 1, &fmt);
	if (QC_PFLOAT(vm, 0))
		localtime_r(&t, &tmv);
	else
		gmtime_r(&t, &tmv);
	if (!strftime(out, sizeof(out), fmt.data, &tmv))
		out[0] = 0;
	QC_ReturnString(vm, out);
	SB_Free(&fmt);
}

static void bi_uri_escape(qcvm_t *vm)
{
	const unsigned char *s = (const unsigned char *)QC_PSTR(vm, 0);
	strbuf_t sb = {0};
	for (; *s; s++) {
		if (isalnum(*s) || *s == '.' || *s == '-' || *s == '_')
			SB_Append(&sb, (const char *)s, 1);
		else
			SB_Printf(&sb, "%%%02X", *s);
	}
	QC_ReturnString(vm, sb.data ? sb.data : "");
	SB_Free(&sb);
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static void bi_uri_unescape(qcvm_t *vm)
{
	const char *s = QC_PSTR(vm, 0);
	strbuf_t sb = {0};
	while (*s) {
		if (s[0] == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
			char ch = (char)(hexval(s[1]) * 16 + hexval(s[2]));
			if (ch)
				SB_Append(&sb, &ch, 1);
			s += 3;
		} else {
			SB_Append(&sb, s++, 1);
		}
	}
	QC_ReturnString(vm, sb.data ? sb.data : "");
	SB_Free(&sb);
}

/* sprintf: %d and %x take floats unless prefixed with l, %i takes ints unless
 * prefixed with h; %s strings, %S quoted strings, %v vectors, %c char codes,
 * %p pointers. Each argument consumes one parameter slot. */
static void bi_sprintf(qcvm_t *vm)
{
	const char *f = QC_PSTR(vm, 0);
	int arg = 1;
	strbuf_t sb = {0};
	while (*f) {
		char spec[32], buf[512];
		int sl = 0;
		bool isint = false, isfloat = false;
		char conv;
		if (*f != '%') {
			const char *e = strchr(f, '%');
			size_t n = e ? (size_t)(e - f) : strlen(f);
			SB_Append(&sb, f, n);
			f += n;
			continue;
		}
		if (f[1] == '%') {
			SB_Append(&sb, "%", 1);
			f += 2;
			continue;
		}
		spec[sl++] = *f++;
		while (*f && strchr("-+ #0", *f) && sl < 20)
			spec[sl++] = *f++;
		if (*f == '*') {
			sl += snprintf(spec + sl, sizeof(spec) - sl, "%d", arg < vm->argc ? (int)QC_PFLOAT(vm, arg) : 0);
			arg++;
			f++;
		}
		while (isdigit((unsigned char)*f) && sl < 24)
			spec[sl++] = *f++;
		if (*f == '.') {
			spec[sl++] = *f++;
			if (*f == '*') {
				sl += snprintf(spec + sl, sizeof(spec) - sl, "%d", arg < vm->argc ? (int)QC_PFLOAT(vm, arg) : 0);
				arg++;
				f++;
			}
			while (isdigit((unsigned char)*f) && sl < 28)
				spec[sl++] = *f++;
		}
		for (;;) {
			if (*f == 'l') {
				isint = true;
				f++;
			} else if (*f == 'h') {
				isfloat = true;
				f++;
			} else
				break;
		}
		conv = *f;
		if (!conv)
			break;
		f++;
		{
			qcval_t *p = arg < vm->argc ? QC_Parm(vm, arg) : NULL;
			qcval_t zero[3] = {{0}};
			if (!p)
				p = zero;
			arg++;
			switch (conv) {
			case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': {
				long long v;
				bool asint = conv == 'i' ? !isfloat : isint;
				v = asint ? p->i : (long long)p->f;
				if (conv == 'c') {
					spec[sl++] = 'c';
					spec[sl] = 0;
					snprintf(buf, sizeof(buf), spec, (int)v);
				} else {
					spec[sl++] = 'l';
					spec[sl++] = 'l';
					spec[sl++] = conv == 'i' ? 'd' : conv;
					spec[sl] = 0;
					snprintf(buf, sizeof(buf), spec, v);
				}
				SB_AppendStr(&sb, buf);
				break;
			}
			case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': {
				double v = isint ? (double)p->i : (double)p->f;
				spec[sl++] = conv;
				spec[sl] = 0;
				snprintf(buf, sizeof(buf), spec, v);
				SB_AppendStr(&sb, buf);
				break;
			}
			case 's': case 'S': {
				const char *s = QC_GetString(vm, p->i);
				if (conv == 'S' && Str_NeedsEscape(s)) {
					/* text a plain quoted string can't hold: \"escaped" */
					SB_Append(&sb, "\\\"", 2);
					for (; *s; s++) {
						const char *e = *s == '\n' ? "\\n" : *s == '\t' ? "\\t" : *s == '\r' ? "\\r" :
						                *s == '"' ? "\\\"" : *s == '\\' ? "\\\\" : NULL;
						if (e)
							SB_AppendStr(&sb, e);
						else
							SB_Append(&sb, s, 1);
					}
					SB_Append(&sb, "\"", 1);
					break;
				}
				if (conv == 'S')
					SB_Append(&sb, "\"", 1);
				spec[sl++] = 's';
				spec[sl] = 0;
				if (sl == 2) {
					SB_AppendStr(&sb, s);
				} else {
					char *big = Mem_Alloc(strlen(s) + 512);
					snprintf(big, strlen(s) + 512, spec, s);
					SB_AppendStr(&sb, big);
					Mem_Free(big);
				}
				if (conv == 'S')
					SB_Append(&sb, "\"", 1);
				break;
			}
			case 'v': {
				char comp[3][128];
				bool hasprec = strchr(spec, '.') != NULL;
				for (int i = 0; i < 3; i++) {
					if (hasprec || sl > 1) {
						char s2[40];
						snprintf(s2, sizeof(s2), "%.*sf", sl, spec);
						snprintf(comp[i], sizeof(comp[i]), s2, (double)p[i].f);
					} else {
						format_float(comp[i], sizeof(comp[i]), p[i].f);
					}
				}
				SB_Printf(&sb, "%s %s %s", comp[0], comp[1], comp[2]);
				break;
			}
			case 'p':
				SB_Printf(&sb, "0x%x", p->u);
				break;
			default:
				SB_Append(&sb, spec, (size_t)sl);
				SB_Append(&sb, &conv, 1);
				arg--;
				break;
			}
		}
	}
	QC_ReturnString(vm, sb.data ? sb.data : "");
	SB_Free(&sb);
}

/* --- tokenize -------------------------------------------------------- */

static void tokens_reset(qcvm_t *vm) { vm->extra->tokens.count = 0; }

static void token_add(qcvm_t *vm, const char *src, const char *s, size_t n, const char *start, const char *end)
{
	qc_tokens_t *t = &vm->extra->tokens;
	if (t->count >= QC_MAX_TOKENS)
		return;
	t->argv[t->count] = QC_NewStringLen(vm, s, n);
	t->start[t->count] = (int)(start - src);
	t->end[t->count] = (int)(end - src);
	t->count++;
}

/* general purpose split: whitespace, double quotes group, single-character
 * tokens for braces and parentheses, // comments end the line */
static void bi_tokenize(qcvm_t *vm)
{
	const char *src = QC_PSTR(vm, 0), *p = src;
	tokens_reset(vm);
	for (;;) {
		const char *start;
		while (*p && (unsigned char)*p <= ' ')
			p++;
		if (!*p)
			break;
		if (p[0] == '/' && p[1] == '/')
			break;
		start = p;
		if (p[0] == '\\' && p[1] == '"') {
			size_t len = strlen(p);
			char *buf = Mem_Alloc(len + 1);
			size_t n = Str_ParseEscaped(&p, buf, len + 1);
			token_add(vm, src, buf, n, start, p);
			Mem_Free(buf);
			continue;
		}
		if (*p == '"') {
			const char *q = ++p;
			while (*p && *p != '"')
				p++;
			token_add(vm, src, q, (size_t)(p - q), start, *p ? p + 1 : p);
			if (*p)
				p++;
			continue;
		}
		if (strchr("{}()", *p)) {
			token_add(vm, src, p, 1, start, p + 1);
			p++;
			continue;
		}
		while ((unsigned char)*p > ' ' && *p != '"' && !strchr("{}()", *p))
			p++;
		token_add(vm, src, start, (size_t)(p - start), start, p);
	}
	QC_ReturnFloat(vm, (float)vm->extra->tokens.count);
}

static void bi_tokenize_console(qcvm_t *vm)
{
	const char *src = QC_PSTR(vm, 0);
	cmd_tokens_t *ct = Mem_Alloc(sizeof(*ct));
	tokens_reset(vm);
	Cmd_TokenizeInto(ct, src);
	for (int i = 0; i < ct->argc; i++)
		token_add(vm, src, ct->argv[i], strlen(ct->argv[i]), src + ct->argstart[i], src + ct->argend[i]);
	Mem_Free(ct);
	QC_ReturnFloat(vm, (float)vm->extra->tokens.count);
}

static void bi_tokenizebyseparator(qcvm_t *vm)
{
	const char *src = QC_PSTR(vm, 0), *p = src, *tokstart = src;
	const char *seps[QC_MAX_EXTPARMS + MAX_PARMS];
	int nseps = 0;
	tokens_reset(vm);
	for (int i = 1; i < vm->argc && nseps < (int)BW_ARRAY_COUNT(seps); i++) {
		const char *s = QC_PSTR(vm, i);
		if (*s)
			seps[nseps++] = s;
	}
	if (!*src) {
		QC_ReturnFloat(vm, 0);
		return;
	}
	while (*p) {
		int hit = -1;
		for (int i = 0; i < nseps; i++)
			if (!strncmp(p, seps[i], strlen(seps[i]))) {
				hit = i;
				break;
			}
		if (hit >= 0) {
			token_add(vm, src, tokstart, (size_t)(p - tokstart), tokstart, p);
			p += strlen(seps[hit]);
			tokstart = p;
		} else {
			p++;
		}
	}
	token_add(vm, src, tokstart, (size_t)(p - tokstart), tokstart, p);
	QC_ReturnFloat(vm, (float)vm->extra->tokens.count);
}

static int token_index(qcvm_t *vm)
{
	int i = (int)QC_PFLOAT(vm, 0);
	if (i < 0)
		i += vm->extra->tokens.count;
	return i;
}

static void bi_argv(qcvm_t *vm)
{
	int i = token_index(vm);
	qc_tokens_t *t = &vm->extra->tokens;
	vm->globals[OFS_RETURN].i = (i >= 0 && i < t->count) ? t->argv[i] : 0;
}

static void bi_argv_start_index(qcvm_t *vm)
{
	int i = token_index(vm);
	qc_tokens_t *t = &vm->extra->tokens;
	QC_ReturnFloat(vm, (i >= 0 && i < t->count) ? (float)t->start[i] : -1);
}

static void bi_argv_end_index(qcvm_t *vm)
{
	int i = token_index(vm);
	qc_tokens_t *t = &vm->extra->tokens;
	QC_ReturnFloat(vm, (i >= 0 && i < t->count) ? (float)t->end[i] : -1);
}

/* --- console, cvars, commands ---------------------------------------- */

static void bi_print(qcvm_t *vm)
{
	strbuf_t sb = {0};
	concat_args(vm, 0, &sb);
	Con_Print(sb.data);
	SB_Free(&sb);
}

static void bi_dprint(qcvm_t *vm)
{
	strbuf_t sb = {0};
	if (!developer)
		return;
	concat_args(vm, 0, &sb);
	Con_Print(sb.data);
	SB_Free(&sb);
}

static void bi_error(qcvm_t *vm)
{
	strbuf_t sb = {0};
	concat_args(vm, 0, &sb);
	QC_RunError(vm, "%s", sb.data);
}

static void bi_objerror(qcvm_t *vm)
{
	strbuf_t sb = {0};
	int self = QC_GInt(vm, vm->gofs.self);
	concat_args(vm, 0, &sb);
	Con_Printf("^1objerror on entity %d (%s): %s\n", self, QC_GetString(vm, QC_EInt(vm, self, vm->fofs.classname)), sb.data);
	SB_Free(&sb);
	QC_FreeEdict(vm, self, QC_GFloat(vm, vm->gofs.time));
}

static void print_edict(qcvm_t *vm, int e)
{
	Con_Printf("entity %d:\n", e);
	for (int i = 0; i < vm->numfielddefs; i++) {
		qcdef_t *d = &vm->fielddefs[i];
		qcval_t *v = QC_EdictFields(vm, e) + d->ofs;
		char buf[512];
		size_t n = strlen(d->name);
		int t = d->type & ~DEF_SAVEGLOBAL;
		/* skip the _x/_y/_z component names and zero values */
		if (n > 2 && d->name[n - 2] == '_' && strchr("xyz", d->name[n - 1]))
			continue;
		if (!v->i && (t != ev_vector || (!v[1].i && !v[2].i)))
			continue;
		QC_FieldToString(vm, e, d, buf, sizeof(buf));
		Con_Printf("  %-20s %s\n", d->name, buf);
	}
}

static void bi_eprint(qcvm_t *vm) { print_edict(vm, QC_PENT(vm, 0)); }
static void bi_coredump(qcvm_t *vm)
{
	for (int e = 0; e < vm->num_edicts; e++)
		if (!vm->edicts[e].free)
			print_edict(vm, e);
}
static void bi_stackdump(qcvm_t *vm) { QC_StackTrace(vm); }
static void bi_noop(qcvm_t *vm) { (void)vm; }
static void bi_crash(qcvm_t *vm) { QC_RunError(vm, "crash() called"); }

static void bi_abort(qcvm_t *vm)
{
	vm->aborting = true;
	vm->abort_value = *QC_Parm(vm, 0);
	if (vm->argc < 1)
		vm->abort_value.i = 0;
	QC_AbortUnwind(vm); /* unwinds like an error, without the report */
}

static void bi_cvar(qcvm_t *vm) { QC_ReturnFloat(vm, Cvar_Value(QC_PSTR(vm, 0))); }
static void bi_cvar_string(qcvm_t *vm) { QC_ReturnString(vm, Cvar_String(QC_PSTR(vm, 0))); }

static void bi_cvar_defstring(qcvm_t *vm)
{
	cvar_t *v = Cvar_Find(QC_PSTR(vm, 0));
	QC_ReturnString(vm, v ? v->defstring : "");
}

static void bi_cvar_description(qcvm_t *vm)
{
	cvar_t *v = Cvar_Find(QC_PSTR(vm, 0));
	QC_ReturnString(vm, v && v->description ? v->description : "");
}

static void bi_cvar_set(qcvm_t *vm) { Cvar_Set(QC_PSTR(vm, 0), QC_PSTR(vm, 1)); }
static void bi_cvar_setf(qcvm_t *vm) { Cvar_SetValue(QC_PSTR(vm, 0), QC_PFLOAT(vm, 1)); }

static void bi_registercvar(qcvm_t *vm)
{
	const char *name = QC_PSTR(vm, 0);
	if (Cvar_Find(name)) {
		QC_ReturnFloat(vm, 0);
		return;
	}
	Cvar_Get(name, QC_PSTR(vm, 1), CVAR_USERCREATED, NULL);
	QC_ReturnFloat(vm, 1);
}

static void bi_cvar_type(qcvm_t *vm)
{
	cvar_t *v = Cvar_Find(QC_PSTR(vm, 0));
	int r = 0;
	if (v) {
		r |= 1;
		if (v->flags & CVAR_ARCHIVE)
			r |= 2;
		if (!(v->flags & CVAR_USERCREATED))
			r |= 8;
		if (v->description)
			r |= 16;
		if (v->flags & CVAR_READONLY)
			r |= 32;
	}
	QC_ReturnFloat(vm, (float)r);
}

static void bi_localcmd(qcvm_t *vm)
{
	strbuf_t sb = {0};
	concat_args(vm, 0, &sb);
	/* game code runs at cmd_gamecodelevel, so restricted commands stay out of its reach */
	Cbuf_AddTextAtLevel(sb.data ? sb.data : "", (int)Cvar_Value("cmd_gamecodelevel"));
	SB_Free(&sb);
}

static void bi_checkcommand(qcvm_t *vm)
{
	const char *n = QC_PSTR(vm, 0);
	QC_ReturnFloat(vm, (Cmd_Exists(n) || Cvar_Find(n)) ? 1 : 0);
}

const char *const qc_supported_extensions[] = {
	"FRIK_FILE", "FTE_STRINGS", "DP_QC_SPRINTF", "DP_QC_TOKENIZEBYSEPARATOR", "DP_QC_FINDFLOAT",
	"DP_QC_FINDFLAGS", "DP_QC_MINMAXBOUND", "DP_QC_SINCOSSQRTPOW", "DP_QC_RANDOMVEC",
	"DP_QC_CVAR_STRING", "DP_QC_CVAR_DEFSTRING", "DP_QC_CVAR_TYPE", "DP_QC_ETOS", "DP_QC_EDICT_NUM",
	"DP_QC_ENTITYDATA", "DP_QC_STRFTIME", "DP_QC_STRREPLACE", "DP_QC_STRING_CASE_FUNCTIONS",
	"DP_QC_STRINGCOLORFUNCTIONS", "DP_QC_URI_ESCAPE", "DP_QC_FS_SEARCH", "DP_REGISTERCVAR",
	"FTE_QC_HASHTABLES", "FTE_QC_INTCONV", "FTE_MEMALLOC", "FTE_QC_CHECKCOMMAND",
	"FTE_QC_CROSSPRODUCT", "FTE_QC_PERSISTENTTEMPSTRINGS", "KRIMZON_SV_PARSECLIENTCOMMAND",
	"EXT_BITSHIFT", NULL};

static void bi_checkextension(qcvm_t *vm)
{
	const char *n = QC_PSTR(vm, 0);
	for (int i = 0; qc_supported_extensions[i]; i++)
		if (!Str_ICmp(qc_supported_extensions[i], n)) {
			QC_ReturnFloat(vm, 1);
			return;
		}
	QC_ReturnFloat(vm, 0);
}

static void bi_checkbuiltin(qcvm_t *vm)
{
	int f = QC_PINT(vm, 0);
	QC_ReturnFloat(vm, QC_BuiltinImplemented(vm, f) ? 1 : 0);
}

static void bi_isfunction(qcvm_t *vm) { QC_ReturnFloat(vm, QC_FindFunction(vm, QC_PSTR(vm, 0)) ? 1 : 0); }

static void bi_callfunction(qcvm_t *vm)
{
	const char *name;
	int f, argc = vm->argc;
	if (argc < 1)
		QC_RunError(vm, "callfunction needs a function name");
	name = QC_PSTR(vm, argc - 1);
	f = QC_FindFunction(vm, name);
	if (!f) {
		/* Nuclide calls this on names that may not exist (spawnfunc_player
		 * in the base game), so a missing function is not an error */
		Con_DPrintf("callfunction: no function named %s\n", name);
		return;
	}
	vm->argc = argc - 1;
	if (QC_FUNC_PROGS(f) == vm->curprogs && vm->functions[QC_FUNC_INDEX(f)].builtin) {
		vm->extra->calling_builtin = QC_FUNC_INDEX(f);
		vm->functions[QC_FUNC_INDEX(f)].builtin(vm);
	} else {
		QC_Execute(vm, f);
	}
}

static void bi_gettime(qcvm_t *vm)
{
	int type = vm->argc ? (int)QC_PFLOAT(vm, 0) : 0;
	static double start;
	double now = Sys_Seconds();
	if (!start)
		start = now;
	if (type == 1 || type == 4) {
		QC_ReturnFloat(vm, (float)(now - start));
	} else if (type == 5) {
		QC_ReturnFloat(vm, (float)time(NULL));
	} else {
		QC_ReturnFloat(vm, (float)(now - start));
	}
}

/* --- entities -------------------------------------------------------- */

static void bi_spawn(qcvm_t *vm) { QC_ReturnInt(vm, QC_SpawnEdict(vm)); }

static void bi_remove(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0);
	if (e <= 0) {
		Con_Printf("^3%s: tried to remove world\n", vm->name);
		return;
	}
	QC_FreeEdict(vm, e, QC_GFloat(vm, vm->gofs.time));
}

static void bi_wasfreed(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0);
	QC_ReturnFloat(vm, (e >= 0 && e < vm->max_edicts && vm->edicts[e].free) ? 1 : 0);
}

static int next_live(qcvm_t *vm, int e)
{
	for (e++; e < vm->num_edicts; e++)
		if (!vm->edicts[e].free)
			return e;
	return 0;
}

static void bi_nextent(qcvm_t *vm) { QC_ReturnInt(vm, next_live(vm, QC_PENT(vm, 0))); }

static void bi_edict_num(qcvm_t *vm)
{
	int n = (int)QC_PFLOAT(vm, 0);
	QC_ReturnInt(vm, (n >= 0 && n < vm->max_edicts) ? n : 0);
}

static void bi_num_for_edict(qcvm_t *vm) { QC_ReturnFloat(vm, (float)QC_PENT(vm, 0)); }

static void bi_find(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0), f = QC_PINT(vm, 1);
	const char *match = QC_PSTR(vm, 2);
	while ((e = next_live(vm, e))) {
		if (!strcmp(QC_GetString(vm, QC_EInt(vm, e, f)), match)) {
			QC_ReturnInt(vm, e);
			return;
		}
	}
	QC_ReturnInt(vm, 0);
}

static void bi_findfloat(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0), f = QC_PINT(vm, 1);
	int32_t match = QC_PINT(vm, 2);
	while ((e = next_live(vm, e))) {
		if (QC_EInt(vm, e, f) == match) {
			QC_ReturnInt(vm, e);
			return;
		}
	}
	QC_ReturnInt(vm, 0);
}

static void bi_findflags(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0), f = QC_PINT(vm, 1);
	int match = (int)QC_PFLOAT(vm, 2);
	while ((e = next_live(vm, e))) {
		if ((int)QC_EFloat(vm, e, f) & match) {
			QC_ReturnInt(vm, e);
			return;
		}
	}
	QC_ReturnInt(vm, 0);
}

static int chain_field(qcvm_t *vm, int argn)
{
	return vm->argc > argn ? QC_PINT(vm, argn) : vm->fofs.chain;
}

static void find_chain(qcvm_t *vm, int mode)
{
	int f = QC_PINT(vm, 0), cf = chain_field(vm, 2), head = 0;
	if (cf < 0)
		QC_RunError(vm, "findchain needs a .chain field");
	for (int e = vm->num_edicts - 1; e > 0; e--) {
		bool hit;
		if (vm->edicts[e].free)
			continue;
		if (mode == 0)
			hit = !strcmp(QC_GetString(vm, QC_EInt(vm, e, f)), QC_PSTR(vm, 1));
		else if (mode == 1)
			hit = QC_EInt(vm, e, f) == QC_PINT(vm, 1);
		else
			hit = ((int)QC_EFloat(vm, e, f) & (int)QC_PFLOAT(vm, 1)) != 0;
		if (hit) {
			QC_SetEInt(vm, e, cf, head);
			head = e;
		}
	}
	QC_ReturnInt(vm, head);
}
static void bi_findchain(qcvm_t *vm) { find_chain(vm, 0); }
static void bi_findchainfloat(qcvm_t *vm) { find_chain(vm, 1); }
static void bi_findchainflags(qcvm_t *vm) { find_chain(vm, 2); }

static void bi_findradius(qcvm_t *vm)
{
	float *org = QC_PVEC(vm, 0), rad = QC_PFLOAT(vm, 1);
	int cf = chain_field(vm, 2), head = 0;
	float r2 = rad * rad;
	if (cf < 0 || vm->fofs.origin < 0)
		QC_RunError(vm, "findradius needs .origin and .chain fields");
	for (int e = vm->num_edicts - 1; e > 0; e--) {
		qcval_t *o;
		float d[3];
		if (vm->edicts[e].free)
			continue;
		if (vm->fofs.solid >= 0 && QC_EFloat(vm, e, vm->fofs.solid) == 0 && vm->kind == QCVM_SERVER)
			continue; /* SOLID_NOT is skipped, as in the classic builtin */
		o = QC_EField(vm, e, vm->fofs.origin);
		for (int i = 0; i < 3; i++) {
			float half = 0;
			if (vm->fofs.mins >= 0 && vm->fofs.maxs >= 0)
				half = (QC_EFloat(vm, e, vm->fofs.mins + i) + QC_EFloat(vm, e, vm->fofs.maxs + i)) * 0.5f;
			d[i] = org[i] - (o[i].f + half);
		}
		if (DotProduct(d, d) > r2)
			continue;
		QC_SetEInt(vm, e, cf, head);
		head = e;
	}
	QC_ReturnInt(vm, head);
}

static void bi_copyentity(qcvm_t *vm)
{
	int from = QC_PENT(vm, 0);
	int to = vm->argc >= 2 ? QC_PENT(vm, 1) : QC_SpawnEdict(vm);
	if (to <= 0 && vm->argc >= 2)
		QC_RunError(vm, "copyentity into world");
	memcpy(QC_EdictFields(vm, to), QC_EdictFields(vm, from), (size_t)vm->edict_words * 4);
	QC_ReturnInt(vm, to);
}

static void bi_numentityfields(qcvm_t *vm) { QC_ReturnFloat(vm, (float)vm->numfielddefs); }

static qcdef_t *field_by_num(qcvm_t *vm, int n)
{
	return (n >= 0 && n < vm->numfielddefs) ? &vm->fielddefs[n] : NULL;
}

static void bi_entityfieldname(qcvm_t *vm)
{
	qcdef_t *d = field_by_num(vm, (int)QC_PFLOAT(vm, 0));
	QC_ReturnString(vm, d ? d->name : "");
}

static void bi_entityfieldtype(qcvm_t *vm)
{
	qcdef_t *d = field_by_num(vm, (int)QC_PFLOAT(vm, 0));
	QC_ReturnFloat(vm, d ? (float)(d->type & ~DEF_SAVEGLOBAL) : 0);
}

/* a field's number by name; 0 (the unnamed first field) when there's none */
static void bi_findentityfield(qcvm_t *vm)
{
	const char *name = QC_PSTR(vm, 0);
	for (int i = 1; i < vm->numfielddefs; i++)
		if (vm->fielddefs[i].name && !strcmp(vm->fielddefs[i].name, name)) {
			QC_ReturnFloat(vm, (float)i);
			return;
		}
	QC_ReturnFloat(vm, 0);
}

static void bi_entityfieldref(qcvm_t *vm)
{
	qcdef_t *d = field_by_num(vm, (int)QC_PFLOAT(vm, 0));
	QC_ReturnInt(vm, d ? (int32_t)d->ofs : 0);
}

static void bi_getentityfieldstring(qcvm_t *vm)
{
	qcdef_t *d = field_by_num(vm, (int)QC_PFLOAT(vm, 0));
	char buf[4096];
	if (!d) {
		QC_ReturnString(vm, "");
		return;
	}
	QC_FieldToString(vm, QC_PENT(vm, 1), d, buf, sizeof(buf));
	QC_ReturnString(vm, buf);
}

static void bi_putentityfieldstring(qcvm_t *vm)
{
	qcdef_t *d = field_by_num(vm, (int)QC_PFLOAT(vm, 0));
	QC_ReturnFloat(vm, d && QC_SetFieldFromString(vm, QC_PENT(vm, 1), d, QC_PSTR(vm, 2)) ? 1 : 0);
}

static void bi_parseentitydata(qcvm_t *vm)
{
	const char *s = QC_PSTR(vm, 1), *end;
	int ofs = vm->argc >= 3 ? (int)QC_PFLOAT(vm, 2) : 0;
	bool ok;
	if (ofs < 0 || ofs > (int)strlen(s)) {
		QC_ReturnFloat(vm, -1);
		return;
	}
	end = QC_ParseEdict(vm, QC_PENT(vm, 0), s + ofs, &ok);
	QC_ReturnFloat(vm, ok && end ? (float)(end - s) : -1);
}

/* --- memory ---------------------------------------------------------- */

static void bi_memalloc(qcvm_t *vm)
{
	int size = QC_PINT(vm, 0);
	/* a zero-byte request still gets a distinct block, as Nuclide checks the result for failure */
	QC_ReturnInt(vm, size >= 0 ? (int32_t)QC_HeapAlloc(vm, (size_t)size) : 0);
}

static void bi_memrealloc(qcvm_t *vm)
{
	int size = QC_PINT(vm, 1);
	QC_ReturnInt(vm, (int32_t)QC_HeapRealloc(vm, (uint32_t)QC_PINT(vm, 0), (size_t)BW_MAX(size, 1)));
}

static void bi_memfree(qcvm_t *vm) { QC_HeapFree(vm, (uint32_t)QC_PINT(vm, 0)); }

static void bi_memcpy(qcvm_t *vm)
{
	uint32_t dst = (uint32_t)QC_PINT(vm, 0), src = (uint32_t)QC_PINT(vm, 1);
	int size = QC_PINT(vm, 2);
	if (vm->argc >= 4)
		dst += (uint32_t)QC_PINT(vm, 3);
	if (vm->argc >= 5)
		src += (uint32_t)QC_PINT(vm, 4);
	if (size <= 0)
		return;
	memmove(QC_Pointer(vm, dst, (size_t)size), QC_Pointer(vm, src, (size_t)size), (size_t)size);
}

static void bi_memcmp(qcvm_t *vm)
{
	uint32_t a = (uint32_t)QC_PINT(vm, 0), b = (uint32_t)QC_PINT(vm, 1);
	int size = QC_PINT(vm, 2);
	if (vm->argc >= 4)
		a += (uint32_t)QC_PINT(vm, 3);
	if (vm->argc >= 5)
		b += (uint32_t)QC_PINT(vm, 4);
	if (size <= 0) {
		QC_ReturnInt(vm, 0);
		return;
	}
	QC_ReturnInt(vm, memcmp(QC_Pointer(vm, a, (size_t)size), QC_Pointer(vm, b, (size_t)size), (size_t)size));
}

static void bi_memfill8(qcvm_t *vm)
{
	uint32_t dst = (uint32_t)QC_PINT(vm, 0);
	int size = QC_PINT(vm, 2);
	if (vm->argc >= 4)
		dst += (uint32_t)QC_PINT(vm, 3);
	if (size > 0)
		memset(QC_Pointer(vm, dst, (size_t)size), QC_PINT(vm, 1), (size_t)size);
}

/* memgetval/memsetval/memptradd take their offset in 32-bit words */
static void bi_memgetval(qcvm_t *vm)
{
	uint32_t p = (uint32_t)QC_PINT(vm, 0) + (uint32_t)(int)QC_PFLOAT(vm, 1) * 4;
	vm->globals[OFS_RETURN] = *(qcval_t *)QC_Pointer(vm, p, 4);
}

static void bi_memsetval(qcvm_t *vm)
{
	uint32_t p = (uint32_t)QC_PINT(vm, 0) + (uint32_t)(int)QC_PFLOAT(vm, 1) * 4;
	*(qcval_t *)QC_Pointer(vm, p, 4) = *QC_Parm(vm, 2);
}

static void bi_memptradd(qcvm_t *vm)
{
	QC_ReturnInt(vm, (int32_t)((uint32_t)QC_PINT(vm, 0) + (uint32_t)(int)QC_PFLOAT(vm, 1) * 4));
}

/* --- table ----------------------------------------------------------- */

const qc_builtin_t qc_builtins_core[] = {
	{"makevectors", bi_makevectors}, {"normalize", bi_normalize}, {"vlen", bi_vlen},
	{"vectoyaw", bi_vectoyaw}, {"vectoangles", bi_vectoangles}, {"vectoangles2", bi_vectoangles},
	{"random", bi_random}, {"randomvec", bi_randomvec}, {"rint", bi_rint}, {"floor", bi_floor},
	{"ceil", bi_ceil}, {"fabs", bi_fabs}, {"sin", bi_sin}, {"cos", bi_cos}, {"tan", bi_tan},
	{"asin", bi_asin}, {"acos", bi_acos}, {"atan", bi_atan}, {"atan2", bi_atan2}, {"sqrt", bi_sqrt},
	{"pow", bi_pow}, {"logarithm", bi_logarithm}, {"log", bi_logarithm}, {"min", bi_min}, {"max", bi_max},
	{"bound", bi_bound}, {"mod", bi_mod}, {"modulo", bi_mod}, {"anglemod", bi_anglemod},
	{"bitshift", bi_bitshift}, {"crossproduct", bi_crossproduct},
	{"rotatevectorsbyangle", bi_rotatevectorsbyangle}, {"rotatevectorsbyvectors", bi_rotatevectorsbyvectors},
	{"vectorvectors", bi_vectorvectors}, {"ftoi", bi_ftoi}, {"itof", bi_itof},

	{"ftos", bi_ftos}, {"vtos", bi_vtos}, {"etos", bi_etos}, {"stof", bi_stof}, {"stov", bi_stov},
	{"digest_hex", bi_digest_hex}, {"crc16", bi_crc16}, {"stoi", bi_stoi}, {"stoh", bi_stoh}, {"itos", bi_itos}, {"htos", bi_htos}, {"strlen", bi_strlen},
	{"memstrsize", bi_memstrsize}, {"strcat", bi_strcat}, {"strzone", bi_strcat},
	{"strunzone", bi_strunzone}, {"substring", bi_substring}, {"strstrofs", bi_strstrofs},
	{"str2chr", bi_str2chr}, {"chr2str", bi_chr2str}, {"strtolower", bi_strtolower},
	{"strtoupper", bi_strtoupper}, {"strreplace", bi_strreplace}, {"strireplace", bi_strireplace},
	{"strncmp", bi_strncmp}, {"strcasecmp", bi_strcasecmp}, {"strncasecmp", bi_strncasecmp},
	{"strtrim", bi_strtrim}, {"strpad", bi_strpad}, {"strdecolorize", bi_strdecolorize},
	{"strlennocol", bi_strlennocol}, {"strconv", bi_strconv}, {"infoget", bi_infoget},
	{"infoadd", bi_infoadd}, {"strftime", bi_strftime}, {"uri_escape", bi_uri_escape},
	{"uri_unescape", bi_uri_unescape}, {"sprintf", bi_sprintf},

	{"tokenize", bi_tokenize}, {"tokenize_console", bi_tokenize_console},
	{"tokenizebyseparator", bi_tokenizebyseparator}, {"argv", bi_argv},
	{"argv_start_index", bi_argv_start_index}, {"argv_end_index", bi_argv_end_index},

	{"print", bi_print}, {"dprint", bi_dprint}, {"error", bi_error}, {"objerror", bi_objerror},
	{"eprint", bi_eprint}, {"coredump", bi_coredump}, {"stackdump", bi_stackdump},
	{"traceon", bi_noop}, {"traceoff", bi_noop}, {"breakpoint", bi_noop}, {"crash", bi_crash},
	{"abort", bi_abort}, {"cvar", bi_cvar}, {"cvar_string", bi_cvar_string},
	{"cvar_defstring", bi_cvar_defstring}, {"cvar_description", bi_cvar_description},
	{"cvar_set", bi_cvar_set}, {"cvar_setf", bi_cvar_setf}, {"registercvar", bi_registercvar},
	{"cvar_type", bi_cvar_type}, {"localcmd", bi_localcmd}, {"checkcommand", bi_checkcommand},
	{"checkextension", bi_checkextension}, {"checkbuiltin", bi_checkbuiltin},
	{"isfunction", bi_isfunction}, {"callfunction", bi_callfunction}, {"gettime", bi_gettime},

	{"spawn", bi_spawn}, {"remove", bi_remove}, {"wasfreed", bi_wasfreed}, {"nextent", bi_nextent},
	{"edict_num", bi_edict_num}, {"num_for_edict", bi_num_for_edict}, {"find", bi_find},
	{"findfloat", bi_findfloat}, {"findflags", bi_findflags}, {"findchain", bi_findchain},
	{"findchainfloat", bi_findchainfloat}, {"findchainflags", bi_findchainflags},
	{"findradius", bi_findradius}, {"copyentity", bi_copyentity},
	{"numentityfields", bi_numentityfields}, {"entityfieldname", bi_entityfieldname},
	{"entityfieldtype", bi_entityfieldtype}, {"getentityfieldstring", bi_getentityfieldstring},
	{"putentityfieldstring", bi_putentityfieldstring}, {"findentityfield", bi_findentityfield},
	{"entityfieldref", bi_entityfieldref}, {"parseentitydata", bi_parseentitydata},

	{"memalloc", bi_memalloc}, {"memrealloc", bi_memrealloc}, {"memfree", bi_memfree},
	{"memcpy", bi_memcpy}, {"memcmp", bi_memcmp}, {"memfill8", bi_memfill8},
	{"memgetval", bi_memgetval}, {"memsetval", bi_memsetval}, {"memptradd", bi_memptradd},
	{NULL, NULL}};
