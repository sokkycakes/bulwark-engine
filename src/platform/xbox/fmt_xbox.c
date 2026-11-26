/* Bulwark engine - number formatting and parsing for the Xbox build.
 * ISC licence, see LICENSE.
 *
 * nxdk's C library (PDCLib) prints nothing for %f, %e and %g (it does not
 * even take the argument off the list), its scanf skips them, and strtod
 * stops the program with an assertion. The engine formats and parses floats
 * everywhere (cvars, entity strings, the console), so bw_xbox.h sends the
 * printf and scanf families and strtod here.
 *
 * Floats print exactly: the double is turned into its full decimal
 * expansion with a small big-number routine, then rounded half to even at
 * the asked-for digit, which is what glibc prints. strtod is exact for the
 * usual cases (up to 19 significant digits and a power of ten up to 22) and
 * within a few ulps otherwise (powers of ten past 1e22). */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- decimal expansion of a double ---- */

#define LIMB 1000000000u
#define MAXLIMBS 130 /* 1100 decimal digits: the longest exact expansion is 767 significant ones */

typedef struct {
	uint32_t l[MAXLIMBS]; /* base 1e9, least significant first */
	int n;
} big_t;

static void big_mul(big_t *b, uint32_t m)
{
	uint64_t carry = 0;
	for (int i = 0; i < b->n; i++) {
		uint64_t v = (uint64_t)b->l[i] * m + carry;
		b->l[i] = (uint32_t)(v % LIMB);
		carry = v / LIMB;
	}
	while (carry && b->n < MAXLIMBS) {
		b->l[b->n++] = (uint32_t)(carry % LIMB);
		carry /= LIMB;
	}
}

/* digits of |v| (finite, not zero) without leading zeros into dig; returns
 * their count and sets *point: v = 0.dig * 10^point */
static int expand(double v, char *dig, int *point)
{
	union {
		double d;
		uint64_t u;
	} x = {v};
	int e2 = (int)((x.u >> 52) & 0x7ff);
	uint64_t mant = x.u & ((1ull << 52) - 1);
	if (e2)
		mant |= 1ull << 52;
	else
		e2 = 1;
	e2 -= 1075; /* v = mant * 2^e2 */
	while (!(mant & 1)) {
		mant >>= 1;
		e2++;
	}
	big_t b;
	b.n = 0;
	while (mant) {
		b.l[b.n++] = (uint32_t)(mant % LIMB);
		mant /= LIMB;
	}
	int scale = 0; /* v = b / 10^scale */
	if (e2 > 0) {
		for (int k = e2; k > 0; k -= 28)
			big_mul(&b, 1u << (k > 28 ? 28 : k));
	} else if (e2 < 0) {
		/* m * 2^-k = m * 5^k / 10^k */
		for (int k = -e2; k > 0; k -= 13) {
			uint32_t p = 1;
			for (int i = 0; i < (k > 13 ? 13 : k); i++)
				p *= 5;
			big_mul(&b, p);
		}
		scale = -e2;
	}
	int n = 0;
	char tmp[16];
	for (int i = b.n - 1; i >= 0; i--) {
		uint32_t l = b.l[i];
		int k = 0;
		for (int j = 0; j < 9; j++, l /= 10)
			tmp[k++] = (char)('0' + l % 10);
		if (i == b.n - 1)
			while (k > 1 && tmp[k - 1] == '0')
				k--;
		while (k)
			dig[n++] = tmp[--k];
	}
	*point = n - scale;
	while (n > 1 && dig[n - 1] == '0')
		n--;
	return n;
}

/* round dig (n digits, value 0.dig * 10^*point) to its first keep digits,
 * half to even; returns the new count (dig may become "1" with *point+1) */
static int round_digits(char *dig, int n, int keep, int *point)
{
	if (keep >= n)
		return n;
	if (keep < 0) {
		dig[0] = '0';
		return 0;
	}
	bool up;
	char d = dig[keep];
	if (d > '5')
		up = true;
	else if (d < '5')
		up = false;
	else {
		bool rest = false;
		for (int i = keep + 1; i < n; i++)
			if (dig[i] != '0')
				rest = true;
		up = rest || (keep > 0 && ((dig[keep - 1] - '0') & 1));
	}
	n = keep;
	if (up) {
		int i = keep - 1;
		while (i >= 0 && dig[i] == '9')
			dig[i--] = '0';
		if (i >= 0)
			dig[i]++;
		else {
			dig[0] = '1';
			n = 1;
			(*point)++;
			return n;
		}
	}
	while (n > 0 && dig[n - 1] == '0')
		n--;
	return n;
}

/* ---- the output buffer ---- */

typedef struct {
	char *s;
	size_t size; /* room in s, terminator included */
	size_t len;  /* what would have been written */
} out_t;

static void put(out_t *o, char c)
{
	if (o->len + 1 < o->size)
		o->s[o->len] = c;
	o->len++;
}

static void putn(out_t *o, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		put(o, s[i]);
}

static void pad(out_t *o, char c, int n)
{
	while (n-- > 0)
		put(o, c);
}

enum { F_MINUS = 1, F_PLUS = 2, F_SPACE = 4, F_ALT = 8, F_ZERO = 16 };

/* body is the digits after the sign and any prefix; zero padding goes between */
static void emit(out_t *o, int flags, int width, const char *sign, const char *body, size_t blen)
{
	int len = (int)(strlen(sign) + blen);
	if (!(flags & F_MINUS) && !(flags & F_ZERO))
		pad(o, ' ', width - len);
	putn(o, sign, strlen(sign));
	if (!(flags & F_MINUS) && (flags & F_ZERO))
		pad(o, '0', width - len);
	putn(o, body, blen);
	if (flags & F_MINUS)
		pad(o, ' ', width - len);
}

#define FBUF 1200

static void fmt_float(out_t *o, double v, int flags, int width, int prec, char conv)
{
	char dig[1100];
	char body[FBUF + 400];
	int bl = 0;
	bool upper = conv >= 'A' && conv <= 'Z';
	char lc = (char)(conv | 0x20);
	union {
		double d;
		uint64_t u;
	} x = {v};
	const char *sign = (x.u >> 63) ? "-" : (flags & F_PLUS) ? "+" : (flags & F_SPACE) ? " " : "";
	if (((x.u >> 52) & 0x7ff) == 0x7ff) {
		const char *t = (x.u & ((1ull << 52) - 1)) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
		emit(o, flags & ~F_ZERO, width, sign, t, 3);
		return;
	}
	if (v < 0)
		v = -v;
	if (prec < 0)
		prec = 6;
	if (prec > 500)
		prec = 500;
	int n = 0, point = 1;
	if (v != 0)
		n = expand(v, dig, &point);
	else
		dig[0] = '0';

	if (lc == 'a') {
		/* hexadecimal: 1.hhh p+e, precision as given or exact */
		uint64_t m = x.u & ((1ull << 52) - 1);
		int e = (int)((x.u >> 52) & 0x7ff);
		int lead = e ? 1 : 0;
		if (!v)
			lead = 0, e = 0;
		else
			e = e ? e - 1023 : -1022;
		int hd = 13;
		if (prec >= 0 && prec < 13 && (flags & 0x100)) {
			/* round to prec hex digits */
			int drop = (13 - prec) * 4;
			uint64_t half = 1ull << (drop - 1), rem = m & ((1ull << drop) - 1);
			m >>= drop;
			if (rem > half || (rem == half && (m & 1)))
				m++;
			if (m >> (prec * 4))
				lead++, m &= (1ull << (prec * 4)) - 1;
			hd = prec;
		} else {
			while (hd > 0 && !(m & 0xf))
				m >>= 4, hd--;
		}
		const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
		body[bl++] = '0';
		body[bl++] = upper ? 'X' : 'x';
		body[bl++] = (char)('0' + lead);
		if (hd || (flags & F_ALT))
			body[bl++] = '.';
		for (int i = hd - 1; i >= 0; i--)
			body[bl++] = hex[(m >> (i * 4)) & 0xf];
		bl += sprintf(body + bl, "%c%+d", upper ? 'P' : 'p', e);
		emit(o, flags, width, sign, body, (size_t)bl);
		return;
	}

	bool strip = false;
	if (lc == 'g') {
		int P = prec ? prec : 1;
		int p2 = point;
		char tmp[1100];
		memcpy(tmp, dig, (size_t)n);
		int m = v != 0 ? round_digits(tmp, n, P, &p2) : 1;
		(void)m;
		int X = v != 0 ? p2 - 1 : 0;
		if (P > X && X >= -4) {
			lc = 'f';
			prec = P - 1 - X;
		} else {
			lc = 'e';
			prec = P - 1;
		}
		strip = !(flags & F_ALT);
	}

	if (lc == 'f') {
		if (v != 0)
			n = round_digits(dig, n, point + prec, &point);
		/* integer part */
		if (point <= 0 || n == 0)
			body[bl++] = '0';
		else
			for (int i = 0; i < point; i++)
				body[bl++] = i < n ? dig[i] : '0';
		if (prec || (flags & F_ALT))
			body[bl++] = '.';
		for (int i = 0; i < prec; i++) {
			int k = point + i;
			body[bl++] = (n && k >= 0 && k < n) ? dig[k] : '0';
		}
	} else { /* e */
		int e = 0;
		if (v != 0) {
			n = round_digits(dig, n, prec + 1, &point);
			e = point - 1;
		}
		body[bl++] = n ? dig[0] : '0';
		if (prec || (flags & F_ALT))
			body[bl++] = '.';
		for (int i = 1; i <= prec; i++)
			body[bl++] = i < n ? dig[i] : '0';
		if (strip) {
			while (bl > 0 && body[bl - 1] == '0' && memchr(body, '.', (size_t)bl))
				bl--;
			if (bl > 0 && body[bl - 1] == '.')
				bl--;
			strip = false;
		}
		body[bl++] = upper ? 'E' : 'e';
		body[bl++] = e < 0 ? '-' : '+';
		if (e < 0)
			e = -e;
		char eb[8];
		int el = 0;
		do
			eb[el++] = (char)('0' + e % 10);
		while ((e /= 10));
		if (el < 2)
			eb[el++] = '0';
		while (el)
			body[bl++] = eb[--el];
	}
	if (strip && memchr(body, '.', (size_t)bl)) {
		while (body[bl - 1] == '0')
			bl--;
		if (body[bl - 1] == '.')
			bl--;
	}
	emit(o, flags, width, sign, body, (size_t)bl);
}

static void fmt_int(out_t *o, uint64_t u, bool neg, int flags, int width, int prec, char conv)
{
	char body[80];
	int bl = 0;
	char d[72];
	int dl = 0;
	unsigned base = conv == 'o' ? 8 : (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : 10;
	const char *hex = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
	while (u) {
		d[dl++] = hex[u % base];
		u /= base;
	}
	const char *sign = neg ? "-" : (flags & F_PLUS) ? "+" : (flags & F_SPACE) ? " " : "";
	char pre[4] = "";
	if ((flags & F_ALT) && dl && base == 16) {
		pre[0] = '0', pre[1] = conv == 'X' ? 'X' : 'x', pre[2] = 0;
	}
	if (prec < 0)
		prec = 1;
	else
		flags &= ~F_ZERO;
	int zeros = prec - dl;
	if ((flags & F_ALT) && base == 8 && zeros <= 0)
		zeros = 1;
	char sp[8];
	snprintf(sp, sizeof(sp), "%s%s", sign, pre);
	while (zeros-- > 0)
		body[bl++] = '0';
	while (dl)
		body[bl++] = d[--dl];
	emit(o, flags, width, sp, body, (size_t)bl);
}

static int format(char *s, size_t size, const char *f, va_list ap)
{
	out_t o = {s, size, 0};
	for (; *f; f++) {
		if (*f != '%') {
			put(&o, *f);
			continue;
		}
		const char *start = f++;
		int flags = 0, width = 0, prec = -1;
		for (;; f++) {
			if (*f == '-')
				flags |= F_MINUS;
			else if (*f == '+')
				flags |= F_PLUS;
			else if (*f == ' ')
				flags |= F_SPACE;
			else if (*f == '#')
				flags |= F_ALT;
			else if (*f == '0')
				flags |= F_ZERO;
			else
				break;
		}
		if (*f == '*') {
			width = va_arg(ap, int);
			if (width < 0)
				flags |= F_MINUS, width = -width;
			f++;
		} else
			while (*f >= '0' && *f <= '9')
				width = width * 10 + (*f++ - '0');
		if (*f == '.') {
			f++;
			prec = 0;
			if (*f == '*') {
				prec = va_arg(ap, int);
				f++;
			} else
				while (*f >= '0' && *f <= '9')
					prec = prec * 10 + (*f++ - '0');
		}
		if (flags & F_MINUS)
			flags &= ~F_ZERO;
		int len = 0; /* 1 char, 2 short, 3 long, 4 long long, 5 size_t / intmax / ptrdiff */
		for (;; f++) {
			if (*f == 'h')
				len = len == 2 ? 1 : 2;
			else if (*f == 'l')
				len = len == 3 ? 4 : 3;
			else if (*f == 'L' || *f == 'q')
				len = 4;
			else if (*f == 'z' || *f == 't')
				len = 5;
			else if (*f == 'j')
				len = 4;
			else if (*f == 'I') {
				/* MSVC's I64 / I32 / I */
				if (f[1] == '6' && f[2] == '4')
					len = 4, f += 2;
				else if (f[1] == '3' && f[2] == '2')
					len = 0, f += 2;
				else
					len = 5;
			} else
				break;
		}
		char c = *f;
		switch (c) {
		case 'd':
		case 'i': {
			int64_t v;
			if (len == 4)
				v = va_arg(ap, long long);
			else if (len == 3)
				v = va_arg(ap, long);
			else if (len == 5)
				v = va_arg(ap, ptrdiff_t);
			else
				v = va_arg(ap, int);
			if (len == 1)
				v = (signed char)v;
			else if (len == 2)
				v = (short)v;
			fmt_int(&o, v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v, v < 0, flags, width, prec, 'd');
			break;
		}
		case 'u':
		case 'o':
		case 'x':
		case 'X': {
			uint64_t v;
			if (len == 4)
				v = va_arg(ap, unsigned long long);
			else if (len == 3)
				v = va_arg(ap, unsigned long);
			else if (len == 5)
				v = va_arg(ap, size_t);
			else
				v = va_arg(ap, unsigned);
			if (len == 1)
				v = (unsigned char)v;
			else if (len == 2)
				v = (unsigned short)v;
			fmt_int(&o, v, false, flags & ~(F_PLUS | F_SPACE), width, prec, c);
			break;
		}
		case 'p':
			fmt_int(&o, (uintptr_t)va_arg(ap, void *), false, F_ALT | (flags & F_MINUS), width, -1, 'p');
			break;
		case 'f':
		case 'F':
		case 'e':
		case 'E':
		case 'g':
		case 'G':
		case 'a':
		case 'A':
			fmt_float(&o, va_arg(ap, double), flags | (prec >= 0 ? 0x100 : 0), width, prec < 0 && (c | 0x20) == 'a' ? 13 : prec, c);
			break;
		case 'c': {
			char ch = (char)va_arg(ap, int);
			emit(&o, flags & ~F_ZERO, width, "", &ch, 1);
			break;
		}
		case 's': {
			const char *str = va_arg(ap, const char *);
			if (!str)
				str = "(null)";
			size_t l = prec >= 0 ? strnlen(str, (size_t)prec) : strlen(str);
			emit(&o, flags & ~F_ZERO, width, "", str, l);
			break;
		}
		case 'n':
			if (len == 4)
				*va_arg(ap, long long *) = (long long)o.len;
			else if (len == 2)
				*va_arg(ap, short *) = (short)o.len;
			else if (len == 1)
				*va_arg(ap, signed char *) = (signed char)o.len;
			else
				*va_arg(ap, int *) = (int)o.len;
			break;
		case '%':
			put(&o, '%');
			break;
		default:
			/* not a conversion: print it as it is */
			putn(&o, start, (size_t)(f - start) + (*f ? 1 : 0));
			if (!*f)
				f--;
			break;
		}
	}
	if (o.size)
		o.s[o.len < o.size ? o.len : o.size - 1] = 0;
	return o.len > INT_MAX ? -1 : (int)o.len;
}

int vsnprintf(char *s, size_t n, const char *f, va_list ap)
{
	return format(s, n, f, ap);
}

int snprintf(char *s, size_t n, const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	int r = format(s, n, f, ap);
	va_end(ap);
	return r;
}

int vsprintf(char *s, const char *f, va_list ap)
{
	return format(s, (size_t)INT_MAX, f, ap);
}

int sprintf(char *s, const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	int r = format(s, (size_t)INT_MAX, f, ap);
	va_end(ap);
	return r;
}

int vfprintf(FILE *fp, const char *f, va_list ap)
{
	char small[1024];
	va_list ap2;
	va_copy(ap2, ap);
	int r = format(small, sizeof(small), f, ap);
	if (r < 0) {
		va_end(ap2);
		return r;
	}
	char *s = small;
	if ((size_t)r >= sizeof(small)) {
		s = malloc((size_t)r + 1);
		if (!s) {
			va_end(ap2);
			return -1;
		}
		format(s, (size_t)r + 1, f, ap2);
	}
	va_end(ap2);
	size_t w = fwrite(s, 1, (size_t)r, fp);
	if (s != small)
		free(s);
	return w == (size_t)r ? r : -1;
}

int fprintf(FILE *fp, const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	int r = vfprintf(fp, f, ap);
	va_end(ap);
	return r;
}

int vprintf(const char *f, va_list ap)
{
	return vfprintf(stdout, f, ap);
}

int printf(const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	int r = vfprintf(stdout, f, ap);
	va_end(ap);
	return r;
}

/* ---- strtod ---- */

static const double pow10_exact[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                     1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

static double scale10(double m, int e)
{
	/* in steps that stay exact as long as they can */
	while (e > 22) {
		m *= 1e22;
		e -= 22;
		if (m > 1.8e308)
			return m;
	}
	while (e < -22) {
		m /= 1e22;
		e += 22;
		if (m == 0)
			return 0;
	}
	return e >= 0 ? m * pow10_exact[e] : m / pow10_exact[-e];
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c |= 0x20;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

static bool match_word(const char *s, const char *w)
{
	for (; *w; s++, w++)
		if ((*s | 0x20) != *w)
			return false;
	return true;
}

double strtod(const char *s, char **end)
{
	const char *p = s;
	while (isspace((unsigned char)*p))
		p++;
	bool neg = false;
	if (*p == '+' || *p == '-')
		neg = *p++ == '-';
	double r;
	if (match_word(p, "inf")) {
		p += match_word(p, "infinity") ? 8 : 3;
		r = __builtin_inf();
		goto done;
	}
	if (match_word(p, "nan")) {
		p += 3;
		if (*p == '(') {
			const char *q = p + 1;
			while (isalnum((unsigned char)*q) || *q == '_')
				q++;
			if (*q == ')')
				p = q + 1;
		}
		r = __builtin_nan("");
		goto done;
	}
	if (p[0] == '0' && (p[1] | 0x20) == 'x' && (hexval(p[2]) >= 0 || (p[2] == '.' && hexval(p[3]) >= 0))) {
		p += 2;
		uint64_t m = 0;
		int e = 0, digits = 0;
		bool sticky = false;
		for (; hexval(*p) >= 0; p++)
			if (digits < 15)
				m = m * 16 + (uint64_t)hexval(*p), digits += m != 0;
			else
				e += 4, sticky |= *p != '0';
		if (*p == '.')
			for (p++; hexval(*p) >= 0; p++)
				if (digits < 15)
					m = m * 16 + (uint64_t)hexval(*p), digits += m != 0, e -= 4;
				else
					sticky |= *p != '0';
		if ((*p | 0x20) == 'p') {
			const char *q = p + 1;
			bool en = false;
			if (*q == '+' || *q == '-')
				en = *q++ == '-';
			if (*q >= '0' && *q <= '9') {
				int x = 0;
				for (; *q >= '0' && *q <= '9'; q++)
					if (x < 100000)
						x = x * 10 + (*q - '0');
				e += en ? -x : x;
				p = q;
			}
		}
		(void)sticky;
		r = __builtin_ldexp((double)m, e);
		goto done;
	}
	{
		uint64_t m = 0;
		int digits = 0, e = 0;
		bool any = false;
		for (; *p >= '0' && *p <= '9'; p++) {
			any = true;
			if (digits < 19) {
				m = m * 10 + (uint64_t)(*p - '0');
				digits += m != 0;
			} else
				e++;
		}
		if (*p == '.') {
			const char *q = p + 1;
			if (any || (*q >= '0' && *q <= '9')) {
				for (p++; *p >= '0' && *p <= '9'; p++) {
					any = true;
					if (digits < 19) {
						m = m * 10 + (uint64_t)(*p - '0');
						digits += m != 0;
						e--;
					}
				}
			}
		}
		if (!any) {
			if (end)
				*end = (char *)s;
			return 0;
		}
		if ((*p | 0x20) == 'e') {
			const char *q = p + 1;
			bool en = false;
			if (*q == '+' || *q == '-')
				en = *q++ == '-';
			if (*q >= '0' && *q <= '9') {
				int x = 0;
				for (; *q >= '0' && *q <= '9'; q++)
					if (x < 100000)
						x = x * 10 + (*q - '0');
				e += en ? -x : x;
				p = q;
			}
		}
		if (!m)
			r = 0;
		else if (m < (1ull << 53) && e >= -22 && e <= 22)
			r = e >= 0 ? (double)m * pow10_exact[e] : (double)m / pow10_exact[-e];
		else if (e < -330)
			r = 0;
		else if (e > 310)
			r = __builtin_inf();
		else if (e < -300) /* keep denormals out of the intermediate steps */
			r = scale10(scale10((double)m, e + 300), -300);
		else
			r = scale10((double)m, e);
		if (r == __builtin_inf() || (r == 0 && m))
			errno = ERANGE;
	}
done:
	if (end)
		*end = (char *)p;
	return neg ? -r : r;
}

float strtof(const char *s, char **end)
{
	return (float)strtod(s, end);
}

long double strtold(const char *s, char **end)
{
	return strtod(s, end);
}

double atof(const char *s)
{
	return strtod(s, NULL);
}

/* ---- sscanf ---- */

static bool in_set(const char *set, const char *setend, bool negate, unsigned char c)
{
	bool hit = false;
	for (const char *q = set; q < setend; q++) {
		if (q + 2 < setend && q[1] == '-' && q != set) {
			if (c >= (unsigned char)q[0] && c <= (unsigned char)q[2])
				hit = true;
			q += 2;
		} else if ((unsigned char)*q == c)
			hit = true;
	}
	return hit != negate;
}

int vsscanf(const char *str, const char *f, va_list ap)
{
	const char *s = str;
	int assigned = 0;
	bool converted = false;
	for (; *f; f++) {
		if (isspace((unsigned char)*f)) {
			while (isspace((unsigned char)*s))
				s++;
			continue;
		}
		if (*f != '%' || f[1] == '%') {
			if (*f == '%')
				f++;
			if (*s != *f)
				goto out;
			s++;
			continue;
		}
		f++;
		bool skip = false;
		if (*f == '*')
			skip = true, f++;
		int width = 0;
		while (*f >= '0' && *f <= '9')
			width = width * 10 + (*f++ - '0');
		int len = 0;
		for (;; f++) {
			if (*f == 'h')
				len = len == 2 ? 1 : 2;
			else if (*f == 'l')
				len = len == 3 ? 4 : 3;
			else if (*f == 'L' || *f == 'q' || *f == 'j')
				len = 4;
			else if (*f == 'z' || *f == 't')
				len = 5;
			else
				break;
		}
		char c = *f;
		if (!c)
			break;
		if (c != 'c' && c != '[' && c != 'n')
			while (isspace((unsigned char)*s))
				s++;
		if (c != 'n' && !*s)
			goto out;
		if (!width)
			width = INT_MAX;
		switch (c) {
		case 'd':
		case 'i':
		case 'u':
		case 'o':
		case 'x':
		case 'X':
		case 'p': {
			char buf[72];
			int bl = 0;
			const char *p = s;
			int base = c == 'o' ? 8 : (c == 'x' || c == 'X' || c == 'p') ? 16 : c == 'i' ? 0 : 10;
			if (bl < width && (*p == '+' || *p == '-'))
				buf[bl++] = *p++;
			if (base == 0 || base == 16) {
				if (bl + 1 < width && p[0] == '0' && (p[1] | 0x20) == 'x' && hexval(p[2]) >= 0) {
					buf[bl++] = *p++, buf[bl++] = *p++;
					base = 16;
				} else if (base == 0)
					base = p[0] == '0' ? 8 : 10;
			}
			while (bl < width && bl < 70) {
				int v = hexval(*p);
				if (v < 0 || v >= base)
					break;
				buf[bl++] = *p++;
			}
			buf[bl] = 0;
			char *e;
			unsigned long long uv = 0;
			long long sv = 0;
			bool sign = c == 'd' || c == 'i';
			if (sign)
				sv = strtoll(buf, &e, base);
			else
				uv = strtoull(buf, &e, base);
			if (e == buf)
				goto out;
			s += e - buf;
			converted = true;
			if (skip)
				break;
			if (c == 'p')
				*va_arg(ap, void **) = (void *)(uintptr_t)uv;
			else if (sign) {
				if (len == 1)
					*va_arg(ap, signed char *) = (signed char)sv;
				else if (len == 2)
					*va_arg(ap, short *) = (short)sv;
				else if (len == 4)
					*va_arg(ap, long long *) = sv;
				else if (len == 3)
					*va_arg(ap, long *) = (long)sv;
				else if (len == 5)
					*va_arg(ap, ptrdiff_t *) = (ptrdiff_t)sv;
				else
					*va_arg(ap, int *) = (int)sv;
			} else {
				if (len == 1)
					*va_arg(ap, unsigned char *) = (unsigned char)uv;
				else if (len == 2)
					*va_arg(ap, unsigned short *) = (unsigned short)uv;
				else if (len == 4)
					*va_arg(ap, unsigned long long *) = uv;
				else if (len == 3)
					*va_arg(ap, unsigned long *) = (unsigned long)uv;
				else if (len == 5)
					*va_arg(ap, size_t *) = (size_t)uv;
				else
					*va_arg(ap, unsigned *) = (unsigned)uv;
			}
			assigned++;
			break;
		}
		case 'f':
		case 'F':
		case 'e':
		case 'E':
		case 'g':
		case 'G':
		case 'a':
		case 'A': {
			char buf[128];
			int n = (int)strnlen(s, (size_t)(width < (int)sizeof(buf) - 1 ? width : (int)sizeof(buf) - 1));
			memcpy(buf, s, (size_t)n);
			buf[n] = 0;
			char *e;
			double v = strtod(buf, &e);
			if (e == buf)
				goto out;
			s += e - buf;
			converted = true;
			if (skip)
				break;
			if (len == 3 || len == 4)
				*va_arg(ap, double *) = v;
			else
				*va_arg(ap, float *) = (float)v;
			assigned++;
			break;
		}
		case 's': {
			char *d = skip ? NULL : va_arg(ap, char *);
			int n = 0;
			while (*s && !isspace((unsigned char)*s) && n < width) {
				if (d)
					*d++ = *s;
				s++, n++;
			}
			if (d)
				*d = 0;
			converted = true;
			if (!skip)
				assigned++;
			break;
		}
		case 'c': {
			if (width == INT_MAX)
				width = 1;
			char *d = skip ? NULL : va_arg(ap, char *);
			int n = 0;
			while (*s && n < width) {
				if (d)
					*d++ = *s;
				s++, n++;
			}
			if (n < width)
				goto out;
			converted = true;
			if (!skip)
				assigned++;
			break;
		}
		case '[': {
			const char *set = ++f;
			bool negate = false;
			if (*set == '^')
				negate = true, set++, f++;
			if (*f == ']')
				f++;
			while (*f && *f != ']')
				f++;
			if (!*f)
				goto out;
			const char *setend = f;
			char *d = skip ? NULL : va_arg(ap, char *);
			int n = 0;
			while (*s && n < width && in_set(set, setend, negate, (unsigned char)*s)) {
				if (d)
					*d++ = *s;
				s++, n++;
			}
			if (!n)
				goto out;
			if (d)
				*d = 0;
			converted = true;
			if (!skip)
				assigned++;
			break;
		}
		case 'n':
			if (!skip) {
				int v = (int)(s - str);
				if (len == 4)
					*va_arg(ap, long long *) = v;
				else if (len == 2)
					*va_arg(ap, short *) = (short)v;
				else if (len == 1)
					*va_arg(ap, signed char *) = (signed char)v;
				else
					*va_arg(ap, int *) = v;
			}
			break;
		default:
			goto out;
		}
	}
out:
	if (!converted && !*s && assigned == 0)
		return EOF;
	return assigned;
}

int sscanf(const char *str, const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	int r = vsscanf(str, f, ap);
	va_end(ap);
	return r;
}
