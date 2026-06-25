#ifndef _ERRNO_H
#define _ERRNO_H
#ifdef __cplusplus
extern "C" {
#endif
int *__n9_errno(void);
#ifdef __cplusplus
}
#endif
#define errno (*__n9_errno())
#define EDOM 33
#define ERANGE 34
#define EINVAL 22
#define EILSEQ 84
#define ENOMEM 12
#endif
