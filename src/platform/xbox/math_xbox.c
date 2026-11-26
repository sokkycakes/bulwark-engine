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

/* JavaScript's Math.cbrt, expm1, log1p, acosh, asinh and atanh, and the
 * scalbn its Float16 conversions use (QuickJS, for the Panorama UI) */
double bwx_scalbn(double x, int n) { return ldexp(x, n); } /* FLT_RADIX is 2 */
float bwx_scalbnf(float x, int n) { return ldexpf(x, n); }

double bwx_cbrt(double x)
{
	if (x == 0 || isnan(x) || isinf(x))
		return x;
	double a = fabs(x), y = pow(a, 1.0 / 3.0);
	y -= (y * y * y - a) / (3 * y * y); /* one Newton step for the last bits */
	return x < 0 ? -y : y;
}

/* near zero, log(1 + x) and exp(x) - 1 lose x's low bits to the rounding
 * of 1 + x and exp(x); dividing by the rounded value's own log gets them back */
double bwx_log1p(double x)
{
	double u = 1 + x;
	if (u == 1)
		return x;
	if (isinf(u))
		return u;
	return log(u) * x / (u - 1);
}

double bwx_expm1(double x)
{
	double u = exp(x);
	if (u == 1)
		return x;
	double um1 = u - 1;
	if (um1 == -1 || isinf(u))
		return um1;
	return um1 * x / log(u);
}

double bwx_asinh(double x)
{
	double a = fabs(x), r;
	if (a > 1e8)
		r = log(a) + M_LN2;
	else
		r = bwx_log1p(a + a * a / (1 + sqrt(1 + a * a)));
	return x < 0 ? -r : r;
}

double bwx_acosh(double x)
{
	if (x > 1e8)
		return log(x) + M_LN2;
	return log(x + sqrt(x * x - 1)); /* NaN below 1, as sqrt gives it */
}

double bwx_atanh(double x)
{
	double a = fabs(x), r = 0.5 * bwx_log1p(2 * a / (1 - a));
	return x < 0 ? -r : r;
}
