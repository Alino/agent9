/* Minimal <strings.h> for cc9 (which lacks it). The case-insensitive compares are
 * implemented in gl9_os_extra.c; ffs is a clang builtin. Some Mesa files include
 * <strings.h> explicitly for these. */
#ifndef GL9_STRINGS_H
#define GL9_STRINGS_H
#ifdef __cplusplus
extern "C" {
#endif
int strcasecmp(const char *, const char *);
int strncasecmp(const char *, const char *, unsigned long);
#ifndef ffs
#define ffs __builtin_ffs
#endif
#ifdef __cplusplus
}
#endif
#endif
