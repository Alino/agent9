// cc9: the floating-point std::to_chars overloads (what std::format needs for
// {:f}/{:g}/{:.3f}). Copied from libcxx/src/charconv.cpp but WITHOUT the
// from_chars side, whose include "shared/fp_bits.h" is absent from this libc++
// checkout. to_chars_floating_point.h is self-contained.
#include <charconv>
#include <cstdio>
#include "include/to_chars_floating_point.h"

_LIBCPP_BEGIN_NAMESPACE_STD

// long double: the vendored to_chars_floating_point.h has no 80-bit path, so these
// used to static_cast<double>(v) — silently DROPPING ~4 decimal digits of precision
// (a long double that differs from its double rounding printed the double's digits,
// so std::to_chars/std::format("{}", ld) didn't round-trip). Route through cc9's
// printf %L* instead, which formats the real 80-bit value. `prec < 0` = no explicit
// precision -> 21 significant digits, enough to round-trip the 64-bit mantissa (not
// the strictly SHORTEST form the plain overload ideally yields, but a correct one).
static to_chars_result cc9_ld_to_chars(char* first, char* last, long double v, chars_format fmt, int prec) {
    int p = prec >= 0 ? prec : 21;   // 21 sig digits round-trips the 64-bit mantissa
    // 5200 covers %Lf of LDBL_MAX (~4933 integer digits) + typical precision.
    char tmp[5200];
    int n;
    // literal formats (no -Wformat-nonliteral risk)
    if (fmt == chars_format::fixed)           n = snprintf(tmp, sizeof tmp, "%.*Lf", p, v);
    else if (fmt == chars_format::scientific) n = snprintf(tmp, sizeof tmp, "%.*Le", p, v);
    else if (fmt == chars_format::hex)        n = snprintf(tmp, sizeof tmp, "%.*La", p, v);
    else                                      n = snprintf(tmp, sizeof tmp, "%.*Lg", p, v);
    // [charconv.to.chars]: hex form has NO "0x" prefix (printf %a emits one). Strip it
    // in place, past an optional leading '-'.
    if (fmt == chars_format::hex && n >= 2) {
        int s = (tmp[0] == '-') ? 1 : 0;
        if (n >= s + 2 && tmp[s] == '0' && (tmp[s + 1] | 0x20) == 'x') {
            for (int i = s; i < n - 2; i++) tmp[i] = tmp[i + 2];
            n -= 2;
        }
    }
    // snprintf returns the FULL (untruncated) length, so bound the copy by BOTH the
    // caller's buffer AND tmp — only tmp[0 .. min(n, sizeof tmp - 1)] was actually
    // stored (an absurd precision that overflows tmp -> value_too_large, not an OOB read).
    if (n < 0 || (size_t)n >= sizeof tmp || (size_t)n > (size_t)(last - first))
        return {last, errc::value_too_large};
    for (int i = 0; i < n; i++) first[i] = tmp[i];
    return {first + n, errc{}};
}

to_chars_result to_chars(char* f, char* l, float v) { return _Floating_to_chars<_Floating_to_chars_overload::_Plain>(f, l, v, chars_format{}, 0); }
to_chars_result to_chars(char* f, char* l, double v) { return _Floating_to_chars<_Floating_to_chars_overload::_Plain>(f, l, v, chars_format{}, 0); }
to_chars_result to_chars(char* f, char* l, long double v) { return cc9_ld_to_chars(f, l, v, chars_format::general, -1); }
to_chars_result to_chars(char* f, char* l, float v, chars_format fmt) { return _Floating_to_chars<_Floating_to_chars_overload::_Format_only>(f, l, v, fmt, 0); }
to_chars_result to_chars(char* f, char* l, double v, chars_format fmt) { return _Floating_to_chars<_Floating_to_chars_overload::_Format_only>(f, l, v, fmt, 0); }
to_chars_result to_chars(char* f, char* l, long double v, chars_format fmt) { return cc9_ld_to_chars(f, l, v, fmt, -1); }
to_chars_result to_chars(char* f, char* l, float v, chars_format fmt, int p) { return _Floating_to_chars<_Floating_to_chars_overload::_Format_precision>(f, l, v, fmt, p); }
to_chars_result to_chars(char* f, char* l, double v, chars_format fmt, int p) { return _Floating_to_chars<_Floating_to_chars_overload::_Format_precision>(f, l, v, fmt, p); }
to_chars_result to_chars(char* f, char* l, long double v, chars_format fmt, int p) { return cc9_ld_to_chars(f, l, v, fmt, p); }
_LIBCPP_END_NAMESPACE_STD
