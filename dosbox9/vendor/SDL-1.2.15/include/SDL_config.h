/* SDL_config.h — SDL 1.2.15 for the cc9 -> 9front amd64 target.
 *
 * SDL's configure can't run against cc9, so this is hand-written (that's what
 * SDL_config_minimal.h exists for). We keep SDL's whole generic layer — the
 * blitters, palette->RGB conversion, and audio format conversion are exactly
 * what DOSBox leans on and what we'd get subtly wrong by hand — and supply
 * only two backends: video/plan9 and audio/plan9.
 *
 * Video does NOT link libdraw: cc9's SysV a.out can't link kencc's libdraw, so
 * we speak the gl9win2 protocol (alacritty9/PROTOCOL.md) over fd 0/1 instead.
 */
#ifndef _SDL_config_h
#define _SDL_config_h

#include "SDL_platform.h"

#define SDL_HAS_64BIT_TYPE 1
#define SDL_BYTEORDER 1234		/* amd64 little-endian */

/* cc9 gives us a real (if minimal) libc — use it rather than SDL's fallbacks. */
#define HAVE_LIBC 1

/* Useful headers */
#define HAVE_SYS_TYPES_H 1
#define HAVE_STDIO_H 1
#define STDC_HEADERS 1
#define HAVE_STDLIB_H 1
#define HAVE_STDARG_H 1
#define HAVE_MALLOC_H 1
#define HAVE_MEMORY_H 1		/* cc9/runtime/include/memory.h — the SVID alias */
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_CTYPE_H 1
#define HAVE_MATH_H 1
#define HAVE_SIGNAL_H 1
/* no alloca.h, no iconv.h, no altivec.h */

/* C library functions cc9 provides */
#define HAVE_MALLOC 1
#define HAVE_CALLOC 1
#define HAVE_REALLOC 1
#define HAVE_FREE 1
#define HAVE_ALLOCA 1
#define HAVE_GETENV 1
#define HAVE_PUTENV 1		/* cc9 fs.c — splits and setenv()s, i.e. it copies */
#define HAVE_UNSETENV 1
#define HAVE_QSORT 1
#define HAVE_ABS 1
#define HAVE_BCOPY 1
#define HAVE_MEMSET 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMCMP 1
#define HAVE_STRLEN 1
#define HAVE_STRDUP 1
#define HAVE_STRCHR 1
#define HAVE_STRRCHR 1
#define HAVE_STRSTR 1
#define HAVE_STRTOL 1
#define HAVE_STRTOUL 1
#define HAVE_STRTOLL 1
#define HAVE_STRTOULL 1
#define HAVE_STRTOD 1
#define HAVE_ATOI 1
#define HAVE_ATOF 1
#define HAVE_STRCMP 1
#define HAVE_STRNCMP 1
#define HAVE_STRCASECMP 1
#define HAVE_STRNCASECMP 1
#define HAVE_SSCANF 1
#define HAVE_SNPRINTF 1
#define HAVE_VSNPRINTF 1
#define HAVE_SETJMP 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_SIGACTION 1
/* NOT in cc9: nanosleep (SDL_Delay falls back to select), strlcpy/strlcat,
 * iconv, getpagesize, sem_timedwait. mprotect exists but is a NO-OP
 * (posix_llvm.c) — must NOT claim HAVE_MPROTECT: SDL would believe it worked. */

/* Subsystems: only what DOSBox actually uses.
 * NB: do NOT set SDL_CDROM_DISABLED/SDL_JOYSTICK_DISABLED. DOSBox calls
 * SDL_Init(...|SDL_INIT_CDROM|SDL_INIT_JOYSTICK) and *_DISABLED makes that
 * SDL_Init fail outright ("SDL not built with cdrom support") rather than
 * report zero devices. Keep the subsystems enabled and let SDL's shipped
 * dummy backends answer "0 drives" / "0 joysticks", which is the truth. */
/* #undef SDL_AUDIO_DISABLED */
/* #undef SDL_CDROM_DISABLED */
/* #undef SDL_JOYSTICK_DISABLED */
#define SDL_CDROM_DUMMY 1
#define SDL_JOYSTICK_DUMMY 1
#define SDL_LOADSO_DISABLED 1		/* no dlopen on Plan 9 */
/* #undef SDL_VIDEO_DISABLED */
/* #undef SDL_THREADS_DISABLED */

/* Backends */
#define SDL_AUDIO_DRIVER_P9 1
#define SDL_VIDEO_DRIVER_P9 1
#define SDL_THREAD_PTHREAD 1		/* cc9 has REAL pthreads */
#define SDL_THREAD_PTHREAD_RECURSIVE_MUTEX 1
#define SDL_TIMER_UNIX 1		/* clock_gettime + select */

/* No GL: llvmpipe would be pointless for DOSBox's software surface path. */
/* #undef SDL_VIDEO_OPENGL */

#endif /* _SDL_config_h */
