#ifndef _LOCALE_H
#define _LOCALE_H
struct lconv {
	char *decimal_point, *thousands_sep, *grouping;
	char *int_curr_symbol, *currency_symbol, *mon_decimal_point, *mon_thousands_sep,
	     *mon_grouping, *positive_sign, *negative_sign;
	char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes,
	     n_sep_by_space, p_sign_posn, n_sign_posn, int_p_cs_precedes, int_p_sep_by_space,
	     int_n_cs_precedes, int_n_sep_by_space, int_p_sign_posn, int_n_sign_posn;
};
#define LC_ALL 6
#define LC_COLLATE 3
#define LC_CTYPE 0
#define LC_MONETARY 4
#define LC_NUMERIC 1
#define LC_TIME 5
/* POSIX xlocale masks (for newlocale) */
#define LC_COLLATE_MASK  (1<<LC_COLLATE)
#define LC_CTYPE_MASK    (1<<LC_CTYPE)
#define LC_MONETARY_MASK (1<<LC_MONETARY)
#define LC_NUMERIC_MASK  (1<<LC_NUMERIC)
#define LC_TIME_MASK     (1<<LC_TIME)
#define LC_MESSAGES_MASK (1<<6)
#define LC_ALL_MASK      0x7f
#define LC_GLOBAL_LOCALE ((locale_t)-1)
/* cc9 is C-locale only: locale_t is an opaque handle; the *_l paths ignore it. */
typedef void *locale_t;
#ifdef __cplusplus
extern "C" {
#endif
struct lconv *localeconv(void);
char *setlocale(int, const char *);
locale_t newlocale(int, const char *, locale_t);
locale_t duplocale(locale_t);
void freelocale(locale_t);
locale_t uselocale(locale_t);
double strtod_l(const char *, char **, locale_t);
float strtof_l(const char *, char **, locale_t);
long double strtold_l(const char *, char **, locale_t);
long strtol_l(const char *, char **, int, locale_t);
unsigned long strtoul_l(const char *, char **, int, locale_t);
#ifdef __cplusplus
}
#endif
#endif
