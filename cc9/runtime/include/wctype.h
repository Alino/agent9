#ifndef _WCTYPE_H
#define _WCTYPE_H
typedef int wint_t;
typedef unsigned long wctype_t;
typedef const int *wctrans_t;
typedef void *locale_t;
#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif
#ifdef __cplusplus
extern "C" {
#endif
int iswalnum(wint_t), iswalpha(wint_t), iswblank(wint_t), iswcntrl(wint_t), iswdigit(wint_t),
    iswgraph(wint_t), iswlower(wint_t), iswprint(wint_t), iswpunct(wint_t), iswspace(wint_t),
    iswupper(wint_t), iswxdigit(wint_t);
wint_t towlower(wint_t), towupper(wint_t);
wctype_t wctype(const char *); int iswctype(wint_t, wctype_t);
wctrans_t wctrans(const char *); wint_t towctrans(wint_t, wctrans_t);
/* BSD xlocale wide variants */
int iswalnum_l(wint_t,locale_t), iswalpha_l(wint_t,locale_t), iswblank_l(wint_t,locale_t),
    iswcntrl_l(wint_t,locale_t), iswdigit_l(wint_t,locale_t), iswgraph_l(wint_t,locale_t),
    iswlower_l(wint_t,locale_t), iswprint_l(wint_t,locale_t), iswpunct_l(wint_t,locale_t),
    iswspace_l(wint_t,locale_t), iswupper_l(wint_t,locale_t), iswxdigit_l(wint_t,locale_t);
wint_t towlower_l(wint_t,locale_t), towupper_l(wint_t,locale_t);
wctype_t wctype_l(const char *, locale_t); int iswctype_l(wint_t, wctype_t, locale_t);
wint_t towctrans_l(wint_t, wctrans_t, locale_t); wctrans_t wctrans_l(const char *, locale_t);
#ifdef __cplusplus
}
#endif
#endif
