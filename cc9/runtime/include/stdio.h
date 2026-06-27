#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 1024
#define FILENAME_MAX 1024
#define FOPEN_MAX 16
#define L_tmpnam 1024
#define TMP_MAX 1024
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
typedef struct _CC9_FILE FILE;
typedef struct { long __pos; } fpos_t;
#ifdef __cplusplus
extern "C" {
#endif
extern FILE *stdin; extern FILE *stdout; extern FILE *stderr;
/* C requires stdin/stdout/stderr to be macros (libc++'s cstdio.pass.cpp checks
 * #ifndef); self-referential macros expand to the extern objects above. */
#define stdin stdin
#define stdout stdout
#define stderr stderr
int printf(const char *, ...); int fprintf(FILE *, const char *, ...);
int sprintf(char *, const char *, ...); int snprintf(char *, size_t, const char *, ...);
int vprintf(const char *, va_list); int vfprintf(FILE *, const char *, va_list);
int vsprintf(char *, const char *, va_list); int vsnprintf(char *, size_t, const char *, va_list);
int asprintf(char **, const char *, ...); int vasprintf(char **, const char *, va_list);
int scanf(const char *, ...); int fscanf(FILE *, const char *, ...); int sscanf(const char *, const char *, ...);
int vscanf(const char *, va_list); int vfscanf(FILE *, const char *, va_list); int vsscanf(const char *, const char *, va_list);
FILE *fopen(const char *, const char *); FILE *freopen(const char *, const char *, FILE *);
FILE *fdopen(int, const char *); int fileno(FILE *);
FILE *fmemopen(void *, size_t, const char *);
int fseeko(FILE *, long, int); long ftello(FILE *);
int fclose(FILE *); int fflush(FILE *);
size_t fread(void *, size_t, size_t, FILE *); size_t fwrite(const void *, size_t, size_t, FILE *);
int fseek(FILE *, long, int); long ftell(FILE *); void rewind(FILE *);
int fgetpos(FILE *, fpos_t *); int fsetpos(FILE *, const fpos_t *);
int fgetc(FILE *); int getc(FILE *); int getchar(void); int ungetc(int, FILE *);
int fputc(int, FILE *); int putc(int, FILE *); int putchar(int);
char *fgets(char *, int, FILE *); int fputs(const char *, FILE *); int puts(const char *);
void setbuf(FILE *, char *); int setvbuf(FILE *, char *, int, size_t);
void clearerr(FILE *); int feof(FILE *); int ferror(FILE *); void perror(const char *);
int remove(const char *); int rename(const char *, const char *);
FILE *tmpfile(void); char *tmpnam(char *);
#ifdef __cplusplus
}
#endif
#endif
