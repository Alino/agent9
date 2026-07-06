#ifndef _ICONV_H
#define _ICONV_H
/* stub iconv: iconv_open always fails, so charset-conversion callers take
 * their "conversion unsupported" branch (nvim: UTF-8-only, which is what a
 * Plan 9 system is anyway). */
typedef void *iconv_t;
#ifdef __cplusplus
extern "C" {
#endif
iconv_t iconv_open(const char *, const char *);
unsigned long iconv(iconv_t, char **, unsigned long *, char **, unsigned long *);
int iconv_close(iconv_t);
#ifdef __cplusplus
}
#endif
#endif
