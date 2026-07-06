#ifndef _STRINGS_H
#define _STRINGS_H
typedef unsigned long __strings_size_t;
#ifdef __cplusplus
extern "C" {
#endif
int strcasecmp(const char *, const char *);
int strncasecmp(const char *, const char *, __strings_size_t);
#ifdef __cplusplus
}
#endif
#endif
