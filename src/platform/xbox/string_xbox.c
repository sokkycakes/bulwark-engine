/* Bulwark engine - the C library's memory and string functions, for the
 * Xbox. nxdk builds PDCLib without optimisation, so its memcpy is a byte
 * loop at -O0 (about a dozen instructions a byte): it was a seventh of the
 * frame. Defining these here keeps the linker from pulling PDCLib's.
 * This file builds with -fno-builtin so the loops below stay loops and are
 * not turned back into calls to the functions they implement.
 * ISC licence, see LICENSE. */
#include <stddef.h>
#include <stdint.h>

/* the builtin names bw_xbox.h gives these: here they are defined */
#undef memcpy
#undef memset
#undef memcmp
#undef strlen

/* rep movsd and rep stosd are the Pentium III's fast block moves, and xemu
 * runs each as one helper call rather than per byte */
static inline void copy_fwd(unsigned char *d, const unsigned char *s, size_t n)
{
	size_t words = n >> 2, bytes = n & 3;
	__asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(words) : : "memory");
	__asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(bytes) : : "memory");
}

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
	copy_fwd(dst, src, n);
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	if (d <= s || d >= s + n) {
		copy_fwd(d, s, n);
	} else {
		/* overlapping with the destination above: copy backwards */
		d += n - 1;
		s += n - 1;
		__asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
	}
	return dst;
}

void *memset(void *dst, int c, size_t n)
{
	unsigned char *d = dst;
	uint32_t v = (unsigned char)c * 0x01010101u;
	size_t words = n >> 2, bytes = n & 3;
	__asm__ volatile("rep stosl" : "+D"(d), "+c"(words) : "a"(v) : "memory");
	__asm__ volatile("rep stosb" : "+D"(d), "+c"(bytes) : "a"(v) : "memory");
	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a, *q = b;
	for (; n >= 4; n -= 4, p += 4, q += 4) {
		uint32_t x, y;
		__builtin_memcpy(&x, p, 4);
		__builtin_memcpy(&y, q, 4);
		if (x != y)
			break;
	}
	for (; n; n--, p++, q++)
		if (*p != *q)
			return *p - *q;
	return 0;
}

size_t strlen(const char *s)
{
	const char *p = s;
	while (*p)
		p++;
	return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
	const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
	while (*p && *p == *q)
		p++, q++;
	return *p - *q;
}

int strncmp(const char *a, const char *b, size_t n)
{
	const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
	for (; n; n--, p++, q++)
		if (*p != *q || !*p)
			return *p - *q;
	return 0;
}

char *strchr(const char *s, int c)
{
	char ch = (char)c;
	for (;; s++) {
		if (*s == ch)
			return (char *)s;
		if (!*s)
			return NULL;
	}
}

void *memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = s;
	for (; n; n--, p++)
		if (*p == (unsigned char)c)
			return (void *)p;
	return NULL;
}

/* the C locale's case mapping, which is the only one the engine uses */
int tolower(int c) { return (unsigned)c - 'A' < 26u ? c + ('a' - 'A') : c; }
int toupper(int c) { return (unsigned)c - 'a' < 26u ? c - ('a' - 'A') : c; }
