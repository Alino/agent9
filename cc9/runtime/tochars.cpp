// cc9: the floating-point std::to_chars overloads (what std::format needs for
// {:f}/{:g}/{:.3f}). Copied from libcxx/src/charconv.cpp but WITHOUT the
// from_chars side, whose include "shared/fp_bits.h" is absent from this libc++
// checkout. to_chars_floating_point.h is self-contained.
#include <charconv>
#include "include/to_chars_floating_point.h"

_LIBCPP_BEGIN_NAMESPACE_STD
to_chars_result to_chars(char* f, char* l, float v) { return _Floating_to_chars<_Floating_to_chars_overload::_Plain>(f, l, v, chars_format{}, 0); }
to_chars_result to_chars(char* f, char* l, double v) { return _Floating_to_chars<_Floating_to_chars_overload::_Plain>(f, l, v, chars_format{}, 0); }
to_chars_result to_chars(char* f, char* l, long double v) { return _Floating_to_chars<_Floating_to_chars_overload::_Plain>(f, l, static_cast<double>(v), chars_format{}, 0); }
to_chars_result to_chars(char* f, char* l, float v, chars_format fmt) { return _Floating_to_chars<_Floating_to_chars_overload::_Format_only>(f, l, v, fmt, 0); }
to_chars_result to_chars(char* f, char* l, double v, chars_format fmt) { return _Floating_to_chars<_Floating_to_chars_overload::_Format_only>(f, l, v, fmt, 0); }
to_chars_result to_chars(char* f, char* l, long double v, chars_format fmt) { return _Floating_to_chars<_Floating_to_chars_overload::_Format_only>(f, l, static_cast<double>(v), fmt, 0); }
to_chars_result to_chars(char* f, char* l, float v, chars_format fmt, int p) { return _Floating_to_chars<_Floating_to_chars_overload::_Format_precision>(f, l, v, fmt, p); }
to_chars_result to_chars(char* f, char* l, double v, chars_format fmt, int p) { return _Floating_to_chars<_Floating_to_chars_overload::_Format_precision>(f, l, v, fmt, p); }
to_chars_result to_chars(char* f, char* l, long double v, chars_format fmt, int p) { return _Floating_to_chars<_Floating_to_chars_overload::_Format_precision>(f, l, static_cast<double>(v), fmt, p); }
_LIBCPP_END_NAMESPACE_STD
