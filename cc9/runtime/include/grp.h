#ifndef _GRP_H
#define _GRP_H
#include <sys/types.h>
/* no group db on Plan 9; getgrnam/getgrgid return 0 */
struct group { char *gr_name; char *gr_passwd; gid_t gr_gid; char **gr_mem; };
#ifdef __cplusplus
extern "C" {
#endif
struct group *getgrnam(const char *);
struct group *getgrgid(gid_t);
int setgroups(int, const gid_t *);
int getgrgid_r(gid_t, struct group *, char *, size_t, struct group **);
#ifdef __cplusplus
}
#endif
#endif
