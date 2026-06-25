#ifndef _UNISTD_H
#define _UNISTD_H
#include <sys/types.h>
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0
#define PATH_MAX 4096
#define NAME_MAX 255
#define _PC_PATH_MAX 4
#define _PC_NAME_MAX 3
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#ifdef __cplusplus
extern "C" {
#endif
long read(int, void *, unsigned long); long write(int, const void *, unsigned long);
long pread(int, void *, unsigned long, long); long pwrite(int, const void *, unsigned long, long);
long lseek(int, long, int);
int close(int); int unlink(const char *); int rmdir(const char *);
int rename(const char *, const char *); int link(const char *, const char *);
int symlink(const char *, const char *); long readlink(const char *, char *, unsigned long);
int access(const char *, int); char *getcwd(char *, unsigned long); int chdir(const char *);
int isatty(int); int fsync(int); int ftruncate(int, long); int truncate(const char *, long);
int dup(int); int dup2(int, int); int unlinkat(int, const char *, int);
int symlinkat(const char *, int, const char *); long readlinkat(int, const char *, char *, unsigned long);
int linkat(int, const char *, int, const char *, int); int renameat(int, const char *, int, const char *);
int mkdirat(int, const char *, unsigned int); int faccessat(int, const char *, int, int);
char *realpath(const char *, char *);
long pathconf(const char *, int);
long fpathconf(int, int);
#ifdef __cplusplus
}
#endif
#endif
