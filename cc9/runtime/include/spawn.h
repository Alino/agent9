#ifndef _SPAWN_H
#define _SPAWN_H
#include <sys/types.h>
/* Real posix_spawn over fork+execve (posix_llvm.c). file_actions is a heap
 * vector of dup2/close/open ops applied in order in the child. attrs are
 * accepted and IGNORED (no signal masks / scheduling to speak of on Plan 9). */
typedef struct { void *__acts; } posix_spawn_file_actions_t;
typedef struct { int __x; } posix_spawnattr_t;
#ifdef __cplusplus
extern "C" {
#endif
int posix_spawn(pid_t *, const char *, const posix_spawn_file_actions_t *,
                const posix_spawnattr_t *, char *const[], char *const[]);
int posix_spawnp(pid_t *, const char *, const posix_spawn_file_actions_t *,
                 const posix_spawnattr_t *, char *const[], char *const[]);
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *, int, int);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *, int);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *, int, const char *, int, mode_t);
#ifdef __cplusplus
}
#endif
#endif
