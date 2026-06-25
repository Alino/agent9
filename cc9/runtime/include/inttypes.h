#ifndef _INTTYPES_H
#define _INTTYPES_H
#include <stdint.h>
typedef struct { intmax_t quot, rem; } imaxdiv_t;
/* amd64 LP64: int32 = "", int64 = "l", intmax/ptr = "l". */
#define PRId8  "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "ld"
#define PRIdMAX "ld"
#define PRIdPTR "ld"
#define PRIi8  "i"
#define PRIi16 "i"
#define PRIi32 "i"
#define PRIi64 "li"
#define PRIiMAX "li"
#define PRIiPTR "li"
#define PRIu8  "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "lu"
#define PRIuMAX "lu"
#define PRIuPTR "lu"
#define PRIo8  "o"
#define PRIo16 "o"
#define PRIo32 "o"
#define PRIo64 "lo"
#define PRIoMAX "lo"
#define PRIoPTR "lo"
#define PRIx8  "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 "lx"
#define PRIxMAX "lx"
#define PRIxPTR "lx"
#define PRIX8  "X"
#define PRIX16 "X"
#define PRIX32 "X"
#define PRIX64 "lX"
#define PRIXMAX "lX"
#define PRIXPTR "lX"
#define PRIdLEAST8 "d"
#define PRIdLEAST16 "d"
#define PRIdLEAST32 "d"
#define PRIdLEAST64 "ld"
#define PRIuLEAST8 "u"
#define PRIuLEAST16 "u"
#define PRIuLEAST32 "u"
#define PRIuLEAST64 "lu"
#define PRIdFAST8 "d"
#define PRIdFAST16 "d"
#define PRIdFAST32 "d"
#define PRIdFAST64 "ld"
#define PRIuFAST8 "u"
#define PRIuFAST16 "u"
#define PRIuFAST32 "u"
#define PRIuFAST64 "lu"
#define PRIxLEAST64 "lx"
#define PRIxFAST64 "lx"
#define PRIoLEAST64 "lo"
#define PRIoFAST64 "lo"
#define PRIXLEAST64 "lX"
#define PRIXFAST64 "lX"
#define PRIiLEAST64 "li"
#define PRIiFAST64 "li"
#ifdef __cplusplus
extern "C" {
#endif
intmax_t imaxabs(intmax_t);
imaxdiv_t imaxdiv(intmax_t, intmax_t);
intmax_t strtoimax(const char *, char **, int);
uintmax_t strtoumax(const char *, char **, int);
#ifdef __cplusplus
}
#endif
#endif
