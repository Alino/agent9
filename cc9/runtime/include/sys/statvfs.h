#ifndef _SYS_STATVFS_H
#define _SYS_STATVFS_H
#include <sys/types.h>
struct statvfs {
  unsigned long f_bsize, f_frsize, f_blocks, f_bfree, f_bavail,
                f_files, f_ffree, f_favail, f_fsid, f_flag, f_flags, f_namemax, f_type;
};
/* Mount flags. cc9 sets f_flags=0, so LLVM's is_local() answers "remote"
 * conservatively — it never matters for compilation. */
#define MNT_LOCAL 0
#define ST_RDONLY 1
#define ST_NOSUID 2
#ifdef __cplusplus
extern "C" {
#endif
int statvfs(const char *, struct statvfs *);
int fstatvfs(int, struct statvfs *);
#ifdef __cplusplus
}
#endif
#endif
