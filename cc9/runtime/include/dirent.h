#ifndef _DIRENT_H
#define _DIRENT_H
#include <sys/types.h>
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DT_WHT 14
/* Plan 9 stat has only dir/file; readdir reports DIR/REG/UNKNOWN — the other
 * DT_* exist so switch statements over d_type compile (never produced). */
struct dirent { ino_t d_ino; unsigned char d_type; char d_name[256]; };
typedef struct __cc9_dir DIR;
#ifdef __cplusplus
extern "C" {
#endif
DIR *opendir(const char *); DIR *fdopendir(int);
struct dirent *readdir(DIR *); int closedir(DIR *);
int dirfd(DIR *); void rewinddir(DIR *);
#ifdef __cplusplus
}
#endif
#ifdef __cplusplus
extern "C" {
#endif
int scandir(const char *, struct dirent ***, int (*)(const struct dirent *),
            int (*)(const struct dirent **, const struct dirent **));
int alphasort(const struct dirent **, const struct dirent **);
#ifdef __cplusplus
}
#endif
#endif
