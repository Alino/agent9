#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *malloc(size_t); void free(void *); void *calloc(size_t, size_t);
void *realloc(void *, size_t); void *aligned_alloc(size_t, size_t);
void abort(void); void exit(int); int atexit(void (*)(void));
long strtol(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
int abs(int); long labs(long);
#ifdef __cplusplus
}
#endif
#endif
