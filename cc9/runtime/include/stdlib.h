#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>
typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;
#ifdef __cplusplus
extern "C" {
#endif
void *malloc(size_t); void free(void *); void *calloc(size_t, size_t);
void *realloc(void *, size_t); void *aligned_alloc(size_t, size_t);
void abort(void); void exit(int); int atexit(void (*)(void));
long strtol(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
long long strtoll(const char *, char **, int);
unsigned long long strtoull(const char *, char **, int);
double strtod(const char *, char **); float strtof(const char *, char **); long double strtold(const char *, char **);
int abs(int); long labs(long); long long llabs(long long);
div_t div(int, int); ldiv_t ldiv(long, long); lldiv_t lldiv(long long, long long);
int atoi(const char *); long atol(const char *);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
#ifdef __cplusplus
}
#endif
#endif
