#ifndef _ASSERT_H
#define _ASSERT_H
#ifdef __cplusplus
extern "C" {
#endif
void abort(void);
void __cc9_assert(const char *, const char *, int);
#ifdef __cplusplus
}
#endif
#undef assert
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : __cc9_assert(#e, __FILE__, __LINE__))
#endif

/* C11 <assert.h> defines static_assert as a macro for _Static_assert. In C23
 * it becomes a keyword (no macro); C++ has it as a keyword throughout. */
#if !defined(__cplusplus) && defined(__STDC_VERSION__) \
    && __STDC_VERSION__ >= 201112L && __STDC_VERSION__ < 202311L
#undef static_assert
#define static_assert _Static_assert
#endif
#endif
