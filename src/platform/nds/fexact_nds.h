/* Bulwark engine - exact float shortcuts for the DS's movement prediction (no FPU: every
 * float operation is a libgcc call there). ISC licence, see LICENSE.
 *
 * Each helper gives the same bits as the float operation it stands for, for every input
 * (NaN included where it says so): the server runs those operations on x86 and the
 * prediction has to match it bit for bit (pmove_nds.c). They only save the soft-float
 * call. Plain C99 without libnds: tools/nds/pmtest.c builds them on the host. */
#ifndef BW_FEXACT_NDS_H
#define BW_FEXACT_NDS_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline uint32_t fx_bits(float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	return u;
}

static inline float fx_float(uint32_t u)
{
	float f;
	memcpy(&f, &u, 4);
	return f;
}

/* orders as the floats do, with -0 equal to +0; not for NaN */
static inline int32_t fx_key(float f)
{
	uint32_t u = fx_bits(f);
	return u & 0x80000000u ? -(int32_t)(u & 0x7fffffffu) : (int32_t)u;
}

static inline bool fx_nan(float f) { return (fx_bits(f) & 0x7fffffffu) > 0x7f800000u; }

/* a < b, a <= b, a == b as IEEE compares them: false when either is NaN */
static inline bool fx_lt(float a, float b) { return !fx_nan(a) && !fx_nan(b) && fx_key(a) < fx_key(b); }
static inline bool fx_le(float a, float b) { return !fx_nan(a) && !fx_nan(b) && fx_key(a) <= fx_key(b); }
static inline bool fx_eq(float a, float b) { return !fx_nan(a) && !fx_nan(b) && fx_key(a) == fx_key(b); }
#define fx_gt(a, b) fx_lt(b, a)
#define fx_ge(a, b) fx_le(b, a)

/* x != 0 (a NaN is not 0) */
static inline bool fx_nonzero(float x) { return (fx_bits(x) & 0x7fffffffu) != 0; }

/* 0.0f - x (fteqcc's unary minus): x with the sign flipped, but -(+0) is +0 */
static inline float fx_qneg(float x)
{
	uint32_t u = fx_bits(x);
	return u == 0 ? x : fx_float(u ^ 0x80000000u);
}

/* +0 + x: x, but -0 becomes +0 */
static inline float fx_plus0(float x) { return fx_bits(x) == 0x80000000u ? 0.0f : x; }

/* sqrtf, correctly rounded as IEEE 754 (and x86's sqrtss) has it: a negative argument
 * gives x86's default NaN. Exhaustively checked against the host's sqrtf by pmtest
 * -selftest. */
static inline float fx_sqrt(float x)
{
	uint32_t u = fx_bits(x), m, q, r, hi, lo;
	int e;
	if (u >= 0x7f800000u) {          /* +inf, NaN, negative */
		if (u == 0x7f800000u)
			return x;                 /* +inf */
		if ((u & 0x7fffffffu) > 0x7f800000u)
			return fx_float(u | 0x00400000u); /* a NaN, quieted */
		if (u == 0x80000000u)
			return x;                 /* -0 */
		return fx_float(0xffc00000u);
	}
	if (u == 0)
		return x;
	e = (int)(u >> 23);
	m = u & 0x7fffffu;
	if (e == 0) {                    /* subnormal: normalise */
		while (!(m & 0x800000u))
			m <<= 1, e--;
		e++;
	} else {
		m |= 0x800000u;
	}
	e -= 127;
	if (e & 1)
		m <<= 1, e--;
	/* m in [2^23, 2^25): q = floor(sqrt(m * 2^23)), the 24-bit result, digit by digit
	 * on the 48-bit radicand hi:lo (16 + 32 bits), unrolled; r stays below 2^27 */
	hi = m >> 9;
	lo = m << 23;
	q = 0, r = 0;
#define FX_SQRT_STEP(w, i) do { \
		uint32_t t_; \
		r = r << 2 | ((w) >> (i) & 3); \
		t_ = q << 2 | 1; \
		q <<= 1; \
		if (r >= t_) \
			r -= t_, q |= 1; \
	} while (0)
	FX_SQRT_STEP(hi, 14); FX_SQRT_STEP(hi, 12); FX_SQRT_STEP(hi, 10); FX_SQRT_STEP(hi, 8);
	FX_SQRT_STEP(hi, 6); FX_SQRT_STEP(hi, 4); FX_SQRT_STEP(hi, 2); FX_SQRT_STEP(hi, 0);
	FX_SQRT_STEP(lo, 30); FX_SQRT_STEP(lo, 28); FX_SQRT_STEP(lo, 26); FX_SQRT_STEP(lo, 24);
	FX_SQRT_STEP(lo, 22); FX_SQRT_STEP(lo, 20); FX_SQRT_STEP(lo, 18); FX_SQRT_STEP(lo, 16);
	FX_SQRT_STEP(lo, 14); FX_SQRT_STEP(lo, 12); FX_SQRT_STEP(lo, 10); FX_SQRT_STEP(lo, 8);
	FX_SQRT_STEP(lo, 6); FX_SQRT_STEP(lo, 4); FX_SQRT_STEP(lo, 2); FX_SQRT_STEP(lo, 0);
#undef FX_SQRT_STEP
	/* r = m * 2^23 - q^2; never a tie: round up when the rest is over q */
	if (r > q)
		q++;
	/* q in [2^23, 2^24], 2^24 only by rounding up: then the carry goes into the exponent */
	return fx_float(((uint32_t)(e / 2 + 127) << 23) + (q - 0x800000u));
}
#endif
