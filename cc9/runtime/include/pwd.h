#ifndef _PWD_H
#define _PWD_H
#include <sys/types.h>
/* cc9 has no passwd db; getpwuid returns 0 so LLVM falls back to $HOME / cwd. */
struct passwd {
  char *pw_name, *pw_passwd; uid_t pw_uid; gid_t pw_gid;
  char *pw_gecos, *pw_dir, *pw_shell;
};
#ifdef __cplusplus
extern "C" {
#endif
struct passwd *getpwuid(uid_t);
int getpwuid_r(uid_t, struct passwd *, char *, unsigned long, struct passwd **);
struct passwd *getpwnam(const char *);
int getpwnam_r(const char *, struct passwd *, char *, unsigned long, struct passwd **);
#ifdef __cplusplus
}
#endif
#endif
