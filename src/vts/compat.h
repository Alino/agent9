/*
 * compat.h — porting shim so vts code compiles both on Plan 9 (6c) and
 * macOS/Linux (clang/gcc).
 *
 * Plan 9's 6c only supports simple #ifdef preprocessing — no #if defined().
 * So we use a convention: -DHOST is passed on macOS/Linux builds. Plan 9
 * builds get the Plan 9 path automatically.
 */
#ifndef VTS_COMPAT_H
#define VTS_COMPAT_H

#ifdef HOST
#  include <stdio.h>
#  include <stdlib.h>
#  include <string.h>
#  include <stdint.h>
#  ifndef nil
#    define nil NULL
#  endif
#  ifndef VTS_HAVE_UCHAR
#    define VTS_HAVE_UCHAR 1
typedef unsigned char  uchar;
typedef unsigned short ushort;
typedef unsigned int   uint;
#  endif
#  ifndef print
#    define print printf
#  endif
#  ifndef snprint
#    define snprint snprintf
#  endif
#else
   /* Plan 9 build */
#  include <u.h>
#  include <libc.h>
#endif

#endif
