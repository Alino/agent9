/* libpsl_smoke — gate for the real libpsl with the built-in public suffix
 * list: the builtin context must exist and must classify correctly, exactly
 * the way LibURL/PublicSuffixData.cpp consumes it. */
#include <libpsl.h>
#include <stdio.h>

int main(void)
{
    const psl_ctx_t *psl = psl_builtin();
    if (!psl) {
        printf("FAIL psl_builtin() == NULL (built without builtin data?)\n");
        return 1;
    }
    if (psl_is_public_suffix(psl, "com") != 1) {
        printf("FAIL: 'com' not a public suffix\n");
        return 1;
    }
    if (psl_is_public_suffix(psl, "example.com") != 0) {
        printf("FAIL: 'example.com' claimed to be a public suffix\n");
        return 1;
    }
    if (psl_is_public_suffix2(psl, "co.uk", PSL_TYPE_ANY) != 1) {
        printf("FAIL: 'co.uk' not a public suffix (psl_is_public_suffix2)\n");
        return 1;
    }
    printf("libpsl smoke: OK (builtin from %d suffixes)\n", psl_suffix_count(psl));
    return 0;
}
