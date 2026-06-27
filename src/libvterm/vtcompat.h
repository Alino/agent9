/*
 * vtcompat.h — Plan 9 (kencc) compatibility shim for libvterm 0.3.3.
 *
 * libvterm targets a hosted C99 environment (stdint/stdbool/stdlib/stdio/
 * stdarg/string). Plan 9's native libc has no such headers, so this single
 * shim supplies the fixed-width types, size_t, bool, and the snprintf
 * aliases libvterm needs, layered over <u.h>/<libc.h>.
 *
 * It is included transitively by everything via vterm.h, so the vendored
 * sources just drop their <std*.h> includes (see PORT-NOTES.md).
 *
 * IMPORTANT: every symbol libvterm references must exist in the *native*
 * libc the final link uses (vts links native lib9p/libthread/libc), hence
 * snprintf->snprint / vsnprintf->vsnprint rather than pulling APE.
 */
#ifndef VTERM_PLAN9_COMPAT_H
#define VTERM_PLAN9_COMPAT_H

/* Plan 9's <u.h>/<libc.h> have no include guards. Consumers (vts/engine.c)
 * include them first by convention; libvterm's own sources don't, so pull
 * them in only when absent. `nil` is defined by <u.h>. */
#ifndef nil
#include <u.h>
#include <libc.h>
#endif

/* Make 6l link the archive automatically for anyone including <vterm.h>
 * (same mechanism lib9p/libdraw use). Harmless when building libvterm.a. */
#pragma lib "libvterm.a"

/* Fixed-width types (Plan 9 amd64: int=4, long=4, pointer=8). */
typedef uchar    uint8_t;
typedef char     int8_t;
typedef ushort   uint16_t;
typedef short    int16_t;
typedef uint     uint32_t;
typedef int      int32_t;
typedef uvlong   uint64_t;
typedef vlong    int64_t;

/* size_t must hold object sizes / pointer differences → pointer-width. */
typedef uintptr  size_t;
typedef uintptr  uintptr_t;
typedef intptr   intptr_t;

/* stdbool */
#ifndef __cplusplus
typedef int bool;
#define true  1
#define false 0
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

/* Plan 9 spells these without the trailing 'f'; same arg order + sane
 * truncating semantics. libvterm only uses them on small control strings. */
#define snprintf  snprint
#define vsnprintf vsnprint

/* libvterm's a few unguarded error paths use stderr/fprintf/exit (hosted
 * libc). Map them to Plan 9 equivalents. (abort() exists natively.) */
#define stderr 2
#define fprintf fprint
#define exit(n) exits((n) ? "error" : nil)

#endif
