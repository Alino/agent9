#ifndef _SYS_FILE_H
#define _SYS_FILE_H
/* BSD flock(2). Plan 9 has no advisory file locks; like cc9's fcntl record
 * locks, flock always "succeeds" — single-writer is the norm on 9front. */
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
#ifdef __cplusplus
extern "C" {
#endif
int flock(int, int);
#ifdef __cplusplus
}
#endif
#endif
