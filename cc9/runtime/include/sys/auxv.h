#ifndef _SYS_AUXV_H
#define _SYS_AUXV_H
#define AT_PAGESZ 6
#ifdef __cplusplus
extern "C" {
#endif
unsigned long getauxval(unsigned long);
#ifdef __cplusplus
}
#endif
#endif
