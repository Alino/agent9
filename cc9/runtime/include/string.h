#ifndef _STRING_H
#define _STRING_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *memcpy(void *, const void *, size_t); void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t); int memcmp(const void *, const void *, size_t);
void *memchr(const void *, int, size_t);
size_t strlen(const char *); size_t strnlen(const char *, size_t);
char *strcpy(char *, const char *); char *strncpy(char *, const char *, size_t);
char *strcat(char *, const char *); char *strncat(char *, const char *, size_t);
int strcmp(const char *, const char *); int strncmp(const char *, const char *, size_t);
int strcoll(const char *, const char *); size_t strxfrm(char *, const char *, size_t);
char *strchr(const char *, int); char *strrchr(const char *, int);
char *strstr(const char *, const char *); char *strpbrk(const char *, const char *);
size_t strspn(const char *, const char *); size_t strcspn(const char *, const char *);
char *strtok(char *, const char *);
char *strtok_r(char *, const char *, char **);
char *strerror(int);
int strerror_r(int, char *, size_t);
char *strdup(const char *); char *strndup(const char *, size_t);
char *strsignal(int);
void *memrchr(const void *, int, size_t);
typedef void *locale_t;
int strcoll_l(const char *, const char *, locale_t);
size_t strxfrm_l(char *, const char *, size_t, locale_t);
#ifdef __cplusplus
}
#endif
/* glibc _GNU_SOURCE exposes these in string.h too */
#include <strings.h>
#endif
