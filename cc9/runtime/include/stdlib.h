#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>
#define MB_CUR_MAX 1
#define RAND_MAX 0x7fffffff
#define EXIT_FAILURE 1
#define EXIT_SUCCESS 0
typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;
#ifdef __cplusplus
extern "C" {
#endif
void *malloc(size_t); void free(void *); void *calloc(size_t, size_t);
void *realloc(void *, size_t); void *aligned_alloc(size_t, size_t);
void abort(void); void exit(int); void _Exit(int); int atexit(void (*)(void));
void quick_exit(int); int at_quick_exit(void (*)(void));
long strtol(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
long long strtoll(const char *, char **, int);
unsigned long long strtoull(const char *, char **, int);
double strtod(const char *, char **); float strtof(const char *, char **); long double strtold(const char *, char **);
int abs(int); long labs(long); long long llabs(long long);
div_t div(int, int); ldiv_t ldiv(long, long); lldiv_t lldiv(long long, long long);
int atoi(const char *); long atol(const char *); long long atoll(const char *); double atof(const char *);
char *getenv(const char *); int setenv(const char *, const char *, int); int unsetenv(const char *);
int system(const char *);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
void *bsearch(const void *, const void *, size_t, size_t, int (*)(const void *, const void *));
int rand(void); void srand(unsigned); long random(void); void srandom(unsigned);
int mblen(const char *, size_t); int mbtowc(wchar_t *, const char *, size_t); int wctomb(char *, wchar_t);
size_t mbstowcs(wchar_t *, const char *, size_t); size_t wcstombs(char *, const wchar_t *, size_t);
#ifdef __cplusplus
}
#endif
#endif
