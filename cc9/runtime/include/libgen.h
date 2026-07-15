/* libgen.h — POSIX path component splitting.
 *
 * Both functions may modify their argument and may return a pointer into it
 * (or to static storage), per POSIX. Callers must pass a writable copy — the
 * classic bug is handing them a string literal. */
#ifndef _LIBGEN_H
#define _LIBGEN_H
#ifdef __cplusplus
extern "C" {
#endif
char *dirname(char *);
char *basename(char *);
#ifdef __cplusplus
}
#endif
#endif
