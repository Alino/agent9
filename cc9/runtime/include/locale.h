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
#ifdef __cplusplus
extern "C" {
#endif
struct lconv *localeconv(void);
char *setlocale(int, const char *);
#ifdef __cplusplus
}
#endif
#endif
