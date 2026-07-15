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
/* process / misc — LLVM Unix .inc surface. cc9 backs getpid/getpagesize/sysconf
 * for real; fork/exec/pipe are stubs (not on a compute path) returning -1. */
pid_t getpid(void); pid_t getppid(void);
uid_t getuid(void); uid_t geteuid(void); gid_t getgid(void); gid_t getegid(void);
int getpagesize(void); long sysconf(int);
unsigned int alarm(unsigned int); unsigned int sleep(unsigned int); int usleep(unsigned int);
int pipe(int[2]); int pipe2(int[2], int); pid_t fork(void); void _exit(int);
int execv(const char *, char *const[]); int execve(const char *, char *const[], char *const[]);
int execvp(const char *, char *const[]);
int execlp(const char *, const char *, ...);
int gethostname(char *, unsigned long); int setsid(void); pid_t getsid(pid_t);
int chown(const char *, uid_t, gid_t); int fchown(int, uid_t, gid_t); int lchown(const char *, uid_t, gid_t);
int dup3(int, int, int);
extern char **environ;
#define _SC_PAGESIZE         30
#define _SC_PAGE_SIZE        30
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PHYS_PAGES       85
#define _SC_ARG_MAX          0
#define _SC_CLK_TCK          2
#define _POSIX_ARG_MAX       4096
#define ARG_MAX              131072
#define _SC_GETPW_R_SIZE_MAX 70
#define _SC_HOST_NAME_MAX    180
int mkstemp(char *);                /* POSIX temp file; libc++ test framework uses it */
#ifdef __cplusplus
}
#endif
#ifdef __cplusplus
extern "C" {
#endif
int setuid(uid_t); int setgid(gid_t); int setgroups(int, const gid_t *);
char *ptsname(int);
int ttyname_r(int, char *, size_t);
int execv(const char *, char *const[]);
int execvp(const char *, char *const[]);
int execve(const char *, char *const[], char *const[]);
#ifdef __cplusplus
}
#endif
#endif
