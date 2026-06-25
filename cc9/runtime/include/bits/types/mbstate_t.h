#ifndef _CC9_MBSTATE_T_H
#define _CC9_MBSTATE_T_H
typedef struct { int __count; union { unsigned int __wch; char __wchb[4]; } __value; } mbstate_t;
#endif
