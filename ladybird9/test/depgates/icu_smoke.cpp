// icu_smoke — link gate for the cross-built ICU (ladybird9 dep).
// Exercises libicuuc (ucnv_open, u_strToUpper) + libicui18n pull-in (ucol_open).
// Built + linked by host/deps/build-icu.sh; runs on-box later.
#include <unicode/ucnv.h>
#include <unicode/ustring.h>
#include <unicode/ucol.h>
#include <stdio.h>

int main() {
    UErrorCode err = U_ZERO_ERROR;
    UConverter* cnv = ucnv_open("UTF-8", &err);
    UChar buf[16] = u"plan9";
    UChar up[16];
    int32_t n = u_strToUpper(up, 16, buf, -1, "en", &err);
    UCollator* coll = ucol_open("en", &err);
    printf("icu_smoke: n=%d err=%s coll=%p\n", (int)n, u_errorName(err), (void*)coll);
    if (coll) ucol_close(coll);
    if (cnv) ucnv_close(cnv);
    return U_FAILURE(err) ? 1 : 0;
}
