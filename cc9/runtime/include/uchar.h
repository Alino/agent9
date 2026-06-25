#ifndef _UCHAR_H
#define _UCHAR_H
/* <cuchar> surface — char8/16/32 <-> multibyte conversion. cc9 is C-locale /
 * UTF-8 pass-through; these are declared (and trivially implemented in n9libc)
 * so <cuchar>'s `using ::mbrtoc16` etc. resolve. */
#include <stddef.h>
#include <bits/types/mbstate_t.h>
#ifndef __cpp_char8_t
typedef unsigned char char8_t;
#endif
#ifdef __cplusplus
extern "C" {
#endif
size_t mbrtoc8(char8_t *, const char *, size_t, mbstate_t *);
size_t c8rtomb(char *, char8_t, mbstate_t *);
size_t mbrtoc16(char16_t *, const char *, size_t, mbstate_t *);
size_t c16rtomb(char *, char16_t, mbstate_t *);
size_t mbrtoc32(char32_t *, const char *, size_t, mbstate_t *);
size_t c32rtomb(char *, char32_t, mbstate_t *);
#ifdef __cplusplus
}
#endif
#endif
