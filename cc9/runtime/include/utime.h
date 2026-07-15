#ifndef _UTIME_H
#define _UTIME_H
/* POSIX utime — thin legacy wrapper over utimes (fs.c). Ladybird's
 * LibCore/System.h includes this unconditionally on POSIX platforms. */
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
struct utimbuf {
	time_t actime;
	time_t modtime;
};
int utime(const char *, const struct utimbuf *);
#ifdef __cplusplus
}
#endif
#endif
