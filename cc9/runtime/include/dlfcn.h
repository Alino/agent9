#ifndef _DLFCN_H
#define _DLFCN_H
/* No dynamic loading on Plan 9 a.out (static only). These let LLVM's
 * DynamicLibrary compile; dlopen returns 0 (failure) at runtime. */
#define RTLD_LAZY   1
#define RTLD_NOW    2
#define RTLD_LOCAL  0
#define RTLD_GLOBAL 0x100
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT    ((void *)-1)
typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } Dl_info;
#ifdef __cplusplus
extern "C" {
#endif
void *dlopen(const char *, int);
int   dlclose(void *);
void *dlsym(void *, const char *);
char *dlerror(void);
int   dladdr(const void *, Dl_info *);
#ifdef __cplusplus
}
#endif
#endif
