#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H
#include <stdint.h>
#include <stddef.h>
/* Canonical POSIX scalar types (LLVM's Unix .inc files include <sys/types.h>
 * for these). sys/stat.h includes this header instead of redefining them. */
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef uint8_t u_int8_t;
typedef uint16_t u_int16_t;
typedef uint32_t u_int32_t;
typedef uint64_t u_int64_t;
typedef long ssize_t;
typedef long off_t;
typedef int pid_t;
typedef unsigned int mode_t;
typedef unsigned long ino_t;
typedef unsigned long dev_t;
typedef unsigned int nlink_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef int id_t;
typedef long blksize_t;
typedef long blkcnt_t;
typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;
typedef long suseconds_t;
typedef long useconds_t;
typedef long key_t;
#endif
