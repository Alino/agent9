#ifndef _SYS_FILE_H
#define _SYS_FILE_H
/* BSD flock(2). Plan 9 has no advisory file locks, so — like cc9's fcntl record
 * locks — flock cannot take one and says so: LOCK_SH/LOCK_EX fail with ENOLCK,
 * LOCK_UN succeeds (we hold nothing). It does NOT report success for a lock it
 * never took; see the rationale in runtime/fs.c. */
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
