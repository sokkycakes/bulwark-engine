/* Bulwark engine - the maths functions nxdk's C library leaves as stubs
 * that stop the program (lrint, lround and log2 and their kin).
 * ISC licence, see LICENSE. bw_xbox.h sends the names here. */
#include <math.h>
#include <xmmintrin.h>

/* round to nearest, ties to even (the FPU's default mode) */
long bwx_lrintf(float x)
{
	return _mm_cvtss_si32(_mm_set_ss(x));
}

long bwx_lrint(double x)
{
	/* the Pentium III's SSE has no double conversions: the x87 does it */
	long r;
	__asm__("fistpl %0" : "=m"(r) : "t"(x) : "st");
	return r;
}

long long bwx_llrintf(float x)
{
	long long r;
	__asm__("fistpll %0" : "=m"(r) : "t"((double)x) : "st");
	return r;
}

long long bwx_llrint(double x)
{
	long long r;
	__asm__("fistpll %0" : "=m"(r) : "t"(x) : "st");
	return r;
}

/* round to nearest, ties away from zero */
static double round_away(double x)
{
	double t = trunc(x);
	if (fabs(x - t) >= 0.5)
		t += x < 0 ? -1.0 : 1.0;
	return t;
}

long bwx_lround(double x) { return (long)round_away(x); }
long bwx_lroundf(float x) { return (long)round_away(x); }
long long bwx_llround(double x) { return (long long)round_away(x); }
long long bwx_llroundf(float x) { return (long long)round_away(x); }

double bwx_log2(double x)
{
	int e;
	if (!(x > 0) || isinf(x))
		return log(x) * 1.4426950408889634; /* NaN, -inf and +inf as log gives them */
	double m = frexp(x, &e); /* x = m * 2^e, m in [0.5, 1) */
	if (m == 0.5)
		return e - 1; /* powers of two come out exact */
	return e + log(m) * 1.4426950408889634;
}

float bwx_log2f(float x)
{
	return (float)bwx_log2(x);
}
