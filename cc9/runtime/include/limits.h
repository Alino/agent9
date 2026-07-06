#ifndef _CC9_LIMITS_H
#define _CC9_LIMITS_H
/* clang's freestanding limits.h has the C limits; add the POSIX extras. */
#include_next <limits.h>
#define SSIZE_MAX  0x7fffffffffffffffL
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#ifndef IOV_MAX
#define IOV_MAX 1024
#endif
#ifndef OPEN_MAX
#define OPEN_MAX 64
#endif
#endif
