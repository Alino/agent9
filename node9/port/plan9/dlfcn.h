#ifndef NODE9_DLFCN_H
#define NODE9_DLFCN_H
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL 0
#define RTLD_DEFAULT ((void*)0)
static void *dlopen(const char *f, int m){ (void)f;(void)m; return 0; }
static void *dlsym(void *h, const char *s){ (void)h;(void)s; return 0; }
static int dlclose(void *h){ (void)h; return 0; }
static char *dlerror(void){ return (char*)"dlopen: unsupported on plan9"; }
#endif
