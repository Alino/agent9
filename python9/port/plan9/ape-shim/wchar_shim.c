/*
 * wchar_shim.c -- wide-char functions APE's libc lacks, for the CPython 3.11
 * 9front port. See wchar.h for scope/rationale. wchar_t is 2-byte unsigned
 * short here; these operate on it as opaque code units (no locale/multibyte
 * semantics -- CPython handles UTF-8/UTF-16 itself).
 */
#define _POSIX_SOURCE
#include <stdlib.h>
#include <string.h>
#include "wchar.h"

size_t
wcslen(const wchar_t *s)
{
	const wchar_t *p = s;
	while (*p)
		p++;
	return (size_t)(p - s);
}

int
wcscmp(const wchar_t *a, const wchar_t *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (int)*a - (int)*b;
}

int
wcsncmp(const wchar_t *a, const wchar_t *b, size_t n)
{
	while (n && *a && *a == *b) {
		a++;
		b++;
		n--;
	}
	if (n == 0)
		return 0;
	return (int)*a - (int)*b;
}

wchar_t *
wcschr(const wchar_t *s, wchar_t c)
{
	for (;; s++) {
		if (*s == c)
			return (wchar_t *)s;
		if (*s == 0)
			return NULL;
	}
}

wchar_t *
wcsrchr(const wchar_t *s, wchar_t c)
{
	const wchar_t *last = NULL;
	for (;; s++) {
		if (*s == c)
			last = s;
		if (*s == 0)
			return (wchar_t *)last;
	}
}

wchar_t *
wcscpy(wchar_t *d, const wchar_t *s)
{
	wchar_t *r = d;
	while ((*d++ = *s++) != 0)
		;
	return r;
}

wchar_t *
wcsncpy(wchar_t *d, const wchar_t *s, size_t n)
{
	wchar_t *r = d;
	while (n && (*d = *s) != 0) {
		d++;
		s++;
		n--;
	}
	while (n--)
		*d++ = 0;
	return r;
}

wchar_t *
wcscat(wchar_t *d, const wchar_t *s)
{
	wchar_t *r = d;
	while (*d)
		d++;
	while ((*d++ = *s++) != 0)
		;
	return r;
}

wchar_t *
wcsncat(wchar_t *d, const wchar_t *s, size_t n)
{
	wchar_t *r = d;
	while (*d)
		d++;
	while (n && *s) {
		*d++ = *s++;
		n--;
	}
	*d = 0;
	return r;
}

wchar_t *
wcsdup(const wchar_t *s)
{
	size_t n = wcslen(s) + 1;
	wchar_t *p = (wchar_t *)malloc(n * sizeof(wchar_t));
	if (p)
		wmemcpy(p, s, n);
	return p;
}

wchar_t *
wcstok(wchar_t *s, const wchar_t *delim, wchar_t **save)
{
	wchar_t *tok;
	if (s == NULL)
		s = *save;
	/* skip leading delimiters */
	while (*s && wcschr(delim, *s))
		s++;
	if (*s == 0) {
		*save = s;
		return NULL;
	}
	tok = s;
	/* scan to next delimiter */
	while (*s && !wcschr(delim, *s))
		s++;
	if (*s) {
		*s = 0;
		*save = s + 1;
	} else {
		*save = s;
	}
	return tok;
}

/*
 * wcstol/wcstoul: numeric literals are ASCII, so fold the wide prefix into a
 * narrow buffer and delegate to APE's strtol/strtoul, then map endptr back.
 */
static long
wide_to_narrow_strtoul(const wchar_t *nptr, wchar_t **endptr, int base, int is_signed)
{
	char buf[80];
	char *nend;
	size_t i = 0;
	const wchar_t *p = nptr;

	/* copy ASCII run (covers optional ws, sign, 0x, digits) */
	while (*p && (unsigned)*p < 128 && i < sizeof(buf) - 1)
		buf[i++] = (char)*p++;
	buf[i] = 0;

	long sval = 0;
	unsigned long uval = 0;
	if (is_signed)
		sval = strtol(buf, &nend, base);
	else
		uval = strtoul(buf, &nend, base);

	if (endptr)
		*endptr = (wchar_t *)nptr + (size_t)(nend - buf);
	return is_signed ? sval : (long)uval;
}

long
wcstol(const wchar_t *nptr, wchar_t **endptr, int base)
{
	return wide_to_narrow_strtoul(nptr, endptr, base, 1);
}

unsigned long
wcstoul(const wchar_t *nptr, wchar_t **endptr, int base)
{
	return (unsigned long)wide_to_narrow_strtoul(nptr, endptr, base, 0);
}

int
wmemcmp(const wchar_t *a, const wchar_t *b, size_t n)
{
	while (n--) {
		if (*a != *b)
			return (int)*a - (int)*b;
		a++;
		b++;
	}
	return 0;
}

wchar_t *
wmemcpy(wchar_t *d, const wchar_t *s, size_t n)
{
	wchar_t *r = d;
	while (n--)
		*d++ = *s++;
	return r;
}

wchar_t *
wmemmove(wchar_t *d, const wchar_t *s, size_t n)
{
	if (d < s) {
		while (n--)
			*d++ = *s++;
	} else {
		d += n;
		s += n;
		while (n--)
			*--d = *--s;
	}
	return d;
}

wchar_t *
wmemset(wchar_t *d, wchar_t c, size_t n)
{
	wchar_t *r = d;
	while (n--)
		*d++ = c;
	return r;
}

wchar_t *
wmemchr(const wchar_t *s, wchar_t c, size_t n)
{
	while (n--) {
		if (*s == c)
			return (wchar_t *)s;
		s++;
	}
	return NULL;
}
