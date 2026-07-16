/* delie_gate — verify the cc9 de-lie fixes at runtime on 9front. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/mman.h>

static int pass = 0, fail = 0;
#define CHECK(name, cond) do { if (cond) { pass++; printf("PASS  %s\n", name); } \
    else { fail++; printf("FAIL  %s\n", name); } } while (0)

int main(void) {
    /* O_APPEND: append must not overwrite from offset 0. */
    {
        const char *p = "/tmp/delie_append";
        unlink(p);
        int fd = open(p, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        write(fd, "AAA", 3); close(fd);
        fd = open(p, O_WRONLY|O_APPEND);
        write(fd, "BBB", 3);          /* appends at EOF -> "AAABBB", offset 6 */
        lseek(fd, 0, SEEK_SET);        /* move offset to START; a per-write append must IGNORE it */
        write(fd, "CC", 2);            /* must STILL land at EOF (tests the per-write seek, not just open-time) */
        close(fd);
        char buf[16]; fd = open(p, O_RDONLY); int n = read(fd, buf, sizeof buf); close(fd);
        buf[n>0?n:0] = 0;
        printf("  O_APPEND file = \"%s\" (want AAABBBCC)\n", buf);
        CHECK("O_APPEND appends every write, even after lseek(0)", n==8 && memcmp(buf,"AAABBBCC",8)==0);
    }

    /* fopen("a") must APPEND, not truncate — stdio bypasses open(2) and Plan 9
     * create(2) always truncates, so this is a distinct path from the open() test. */
    {
        const char *p = "/tmp/delie_fappend"; unlink(p);
        FILE *w = fopen(p, "w"); if (w) { fputs("HEAD", w); fclose(w); }
        FILE *a = fopen(p, "a"); if (a) { fputs("TAIL", a); fclose(a); }
        char buf[16]; FILE *r = fopen(p, "r"); int n = r ? (int)fread(buf, 1, sizeof buf, r) : 0; if (r) fclose(r);
        buf[n>0?n:0] = 0;
        printf("  fopen(\"a\") file = \"%s\" (want HEADTAIL)\n", buf);
        CHECK("fopen(a) appends, does not truncate", n==8 && memcmp(buf,"HEADTAIL",8)==0);
    }

    /* printf %.500f: no crash, no OOB (was a 400-byte-buffer over-read). */
    {
        char big[700];
        int n = snprintf(big, sizeof big, "%.500f", 1.0);
        printf("  snprintf(%%.500f,1.0) -> len %d, starts \"%.4s\"\n", n, big);
        CHECK("printf %.500f no OOB/crash", n > 400 && big[0]=='1' && big[1]=='.');
    }

    /* sscanf %hd into a short (was a 4-byte write) + field width. */
    {
        /* guard field directly after the short catches the old 4-byte store */
        struct { short s; short guard; } h = { -1, 0x5a5a };
        int r1 = sscanf("5", "%hd", &h.s);
        int x = -1; int r2 = sscanf("12345", "%2d", &x);
        printf("  sscanf %%hd -> %d (guard=%04x), %%2d -> %d\n", (int)h.s, (unsigned short)h.guard, x);
        CHECK("sscanf %hd stores a short 5 without clobbering the next 2 bytes", r1==1 && h.s==5 && h.guard==0x5a5a);
        CHECK("sscanf %2d honors width (=12)", r2==1 && x==12);
    }

    /* strtol overflow -> LONG_MAX + ERANGE (was wrapped negative, errno clear). */
    {
        errno = 0;
        long v = strtol("9999999999999999999", 0, 10);   /* > LONG_MAX, < ULLONG_MAX */
        printf("  strtol(overflow) -> %ld, errno=%d (want LONG_MAX + ERANGE)\n", v, errno);
        CHECK("strtol overflow -> LONG_MAX+ERANGE", v==LONG_MAX && errno==ERANGE);
    }

    /* access() honors mode: X_OK on a data file must fail. */
    {
        const char *p = "/tmp/delie_data"; unlink(p);
        int fd = open(p, O_WRONLY|O_CREAT|O_TRUNC, 0644); write(fd,"x",1); close(fd);
        int r = access(p, 1 /*X_OK*/);
        printf("  access(data, X_OK) -> %d (want -1)\n", r);
        CHECK("access X_OK fails on a non-exec file", r == -1);
    }

    /* realpath: normalize .. and reject a nonexistent path. */
    {
        char out[1024];
        char *r1 = realpath("/tmp/../tmp", out);
        printf("  realpath(/tmp/../tmp) -> %s\n", r1?r1:"(null)");
        CHECK("realpath normalizes ..", r1 && strcmp(r1,"/tmp")==0);
        errno = 0;
        char *r2 = realpath("/tmp/definitely_not_here_12345", out);
        CHECK("realpath(nonexistent) -> NULL", r2 == 0);
    }

    /* mkdtemp: two calls give distinct (non-fixed-seed) names. */
    {
        char a[] = "/tmp/delie_XXXXXX", b[] = "/tmp/delie_XXXXXX";
        char *ra = mkdtemp(a), *rb = mkdtemp(b);
        printf("  mkdtemp -> %s , %s\n", ra?ra:"(null)", rb?rb:"(null)");
        CHECK("mkdtemp distinct names", ra && rb && strcmp(a,b)!=0);
        if (ra) rmdir(ra); if (rb) rmdir(rb);
    }

    /* mmap(MAP_SHARED) write-back: modify a mapping, msync, reopen, see the change. */
    {
        const char *p = "/tmp/delie_mmap"; unlink(p);
        int fd = open(p, O_RDWR|O_CREAT|O_TRUNC, 0644);
        char init[8] = "01234567"; write(fd, init, 8);
        void *m = mmap(0, 8, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if (m != MAP_FAILED) {
            ((char*)m)[0] = 'Z';
            msync(m, 8, 0); munmap(m, 8);
        }
        close(fd);
        char rb[8]; fd = open(p, O_RDONLY); int n = read(fd, rb, 8); close(fd);
        printf("  mmap MAP_SHARED write-back: byte0='%c' (want Z), mapped=%s\n",
               n>0?rb[0]:'?', m!=MAP_FAILED?"yes":"FAILED-honestly");
        /* PASS either if write-back worked, or mmap honestly failed (no silent loss) */
        CHECK("mmap MAP_SHARED persists or fails honestly", (m!=MAP_FAILED && n==8 && rb[0]=='Z') || m==MAP_FAILED);
    }

    printf("----\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
