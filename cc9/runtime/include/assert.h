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
#endif
