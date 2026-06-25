#ifndef _CTYPE_H
#define _CTYPE_H
#ifdef __cplusplus
extern "C" {
#endif
int isalnum(int), isalpha(int), isblank(int), iscntrl(int), isdigit(int), isgraph(int),
    islower(int), isprint(int), ispunct(int), isspace(int), isupper(int), isxdigit(int),
    tolower(int), toupper(int);
/* BSD xlocale per-locale variants (libc++ locale_base_api expects them here) */
typedef void *locale_t;
int isalnum_l(int,locale_t), isalpha_l(int,locale_t), isblank_l(int,locale_t),
    iscntrl_l(int,locale_t), isdigit_l(int,locale_t), isgraph_l(int,locale_t),
    islower_l(int,locale_t), isprint_l(int,locale_t), ispunct_l(int,locale_t),
    isspace_l(int,locale_t), isupper_l(int,locale_t), isxdigit_l(int,locale_t),
    tolower_l(int,locale_t), toupper_l(int,locale_t);
#ifdef __cplusplus
}
#endif
#endif
