#ifndef __APE_SHIM_WCHAR_H
#define __APE_SHIM_WCHAR_H
/*
 * Minimal <wchar.h> for the 9front/APE CPython 3.11 port.
 *
 * APE's libc predates C95 wide-char support and ships no <wchar.h>. This
 * provides the pure wide-string surface CPython's core actually references on
 * the non-Windows / non-__APPLE__ path (the wide-ctype + btowc workaround in
 * pyport.h is __APPLE__-only and never compiled here).
 *
 * Deliberately absent: the multibyte conversion family (mbrtowc, wcstombs,
 * btowc, ...). pyconfig.h undefs HAVE_MBRTOWC so CPython uses its own internal
 * UTF-8 fallbacks in Python/fileutils.c. wchar_t is APE's 2-byte unsigned short
 * (SIZEOF_WCHAR_T 2 in pyconfig.h -> CPython's UTF-16 wchar handling).
 *
 * Implementations live in wchar_shim.c.
 */

/* plan9: must match pyconfig.h -- force 4-byte wchar_t (kencc L"" / Py_UCS4)
 * BEFORE <stddef.h> so the shim's wcs* functions agree with CPython. Otherwise
 * APE's 2-byte wchar_t makes wcslen() read a 4-byte char as two 2-byte ones. */
#ifndef _WCHAR_T
#define _WCHAR_T
typedef unsigned int wchar_t;
#endif
#include <stddef.h>   /* size_t, NULL */

typedef int wint_t;
#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

#ifndef __APE_SHIM_MBSTATE_T
#define __APE_SHIM_MBSTATE_T
typedef struct { int __count; unsigned long __value; } mbstate_t;
#endif

/* wide string ops */
extern size_t   wcslen(const wchar_t *);
extern int      wcscmp(const wchar_t *, const wchar_t *);
extern int      wcsncmp(const wchar_t *, const wchar_t *, size_t);
extern wchar_t *wcschr(const wchar_t *, wchar_t);
extern wchar_t *wcsrchr(const wchar_t *, wchar_t);
extern wchar_t *wcscpy(wchar_t *, const wchar_t *);
extern wchar_t *wcsncpy(wchar_t *, const wchar_t *, size_t);
extern wchar_t *wcscat(wchar_t *, const wchar_t *);
extern wchar_t *wcsncat(wchar_t *, const wchar_t *, size_t);
extern wchar_t *wcsdup(const wchar_t *);
extern wchar_t *wcstok(wchar_t *, const wchar_t *, wchar_t **);
extern long          wcstol(const wchar_t *, wchar_t **, int);
extern unsigned long wcstoul(const wchar_t *, wchar_t **, int);

/* wide memory ops */
extern int      wmemcmp(const wchar_t *, const wchar_t *, size_t);
extern wchar_t *wmemcpy(wchar_t *, const wchar_t *, size_t);
extern wchar_t *wmemmove(wchar_t *, const wchar_t *, size_t);
extern wchar_t *wmemset(wchar_t *, wchar_t, size_t);
extern wchar_t *wmemchr(const wchar_t *, wchar_t, size_t);

#endif /* __APE_SHIM_WCHAR_H */
