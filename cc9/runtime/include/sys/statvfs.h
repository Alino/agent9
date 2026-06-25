#ifndef _SYS_STATVFS_H
#define _SYS_STATVFS_H
#include <sys/types.h>
struct statvfs {
  unsigned long f_bsize, f_frsize, f_blocks, f_bfree, f_bavail,
                f_files, f_ffree, f_favail, f_fsid, f_flag, f_namemax;
};
#ifdef __cplusplus
extern "C" {
#endif
int statvfs(const char *, struct statvfs *);
int fstatvfs(int, struct statvfs *);
#ifdef __cplusplus
}
#endif
#endif
