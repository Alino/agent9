/* cc9 stdio — a FILE layer over Plan 9 fds, enough to back libc++'s
 * std::cout/cin/cerr (std_stream.h wraps a FILE* with fwrite/fread/fgetc/
 * ungetc/fflush) and the C stdio surface.
 *
 * Buffering is ported from musl (src/stdio/{fread,fwrite,fflush,fseek,setvbuf,
 * ungetc,__toread,__towrite,__uflow,__overflow,__stdio_read,__stdio_write}.c
 * and src/internal/stdio_impl.h): same rpos/rend read window, same
 * wbase/wpos/wend write window, same UNGET slack reserved below buf.
 *
 * It replaces a layer that was genuinely unbuffered: fgetc did ONE pread per
 * byte, fputc ONE pwrite per byte, and setvbuf/setbuf accepted a buffer and a
 * mode, discarded both, and returned success — so every caller (Python sets
 * its streams up at startup) believed it had buffering it never got. A Plan 9
 * syscall is ~1.4us, which made fgets over 1MB ~1.4s of pure kernel entry.
 *
 * Deliberate divergences from musl:
 *  - No f->read/f->write/f->seek cookie pointers. cc9 has exactly one kind of
 *    fd-backed stream, so the syscalls are called directly.
 *  - musl's __stdio_write writev()s [pending buffer, caller's bytes] as one
 *    vector. Plan 9 has no scatter/gather write (cc9's writev is itself a
 *    staging shim), so we drain the buffer and then the caller's bytes with
 *    two loops: same bytes, same order, one extra syscall on write-through.
 *  - musl's __stdio_read readv()s [caller's buffer, f->buf] to satisfy the
 *    request and refill in one syscall. Without readv we split: a request at
 *    least as big as the buffer reads straight into the caller's memory,
 *    anything smaller fills f->buf with one pread and copies out.
 *  - fmemopen streams keep the old direct mem/mpos/msize path — the caller's
 *    array already IS the buffer, there is nothing to interpose.
 *  - musl decides stdout's line-vs-full buffering with a TIOCGWINSZ ioctl;
 *    Plan 9 has no ioctl, so we ask fd2path whether the fd is the console.
 *  - Locking is a per-FILE spin+sem (no flockfile/funlockfile surface). It is
 *    self-defence, not a feature: two threads in `*f->wpos++` can otherwise
 *    race wpos past wend and scribble off the end of the heap block. The old
 *    per-byte layer could interleave bytes but never corrupt memory.
 */
typedef unsigned long size_t;
extern long n9_pwrite(int, const void *, long, long long);
extern long n9_pread(int, void *, long, long long);
extern long n9_seek(long long *, int, long long, int);
extern long n9_fd2path(int, char *, int);
extern void *malloc(size_t);
extern void free(void *);
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern int strcmp(const char *, const char *);
extern int atexit(void (*)(void));
extern int n9_semacquire(int *, int);
extern int n9_semrelease(int *, int);

#define UNGET 8              /* musl: pushback slack reserved below buf */
#define CC9_BUFSIZ 4096      /* MUST match BUFSIZ in <stdio.h>: setbuf() sizes the caller's array by it */
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

/* Per-object lock, same shape as n9libc's malloc lock: atomic test-and-set
 * fast path (no syscall when uncontended, the common case), short spin, only
 * then a real sem sleep. Each lock needs its OWN sem — one shared sem would
 * hand the wakeup token to a waiter on some other object and the rightful
 * waiter would sleep forever. */
struct lk { int held, waiters, sem; };
static void lock(struct lk *l){
	if(__sync_bool_compare_and_swap(&l->held, 0, 1)) return;
	for(int i = 0; i < 256; i++)
		if(__sync_bool_compare_and_swap(&l->held, 0, 1)) return;
	__sync_fetch_and_add(&l->waiters, 1);
	while(!__sync_bool_compare_and_swap(&l->held, 0, 1))
		n9_semacquire(&l->sem, 1);        /* sleep; stale tokens just re-loop */
	__sync_fetch_and_sub(&l->waiters, 1);
}
static void unlock(struct lk *l){
	__sync_lock_release(&l->held);
	if(__sync_fetch_and_add(&l->waiters, 0) > 0)
		n9_semrelease(&l->sem, 1);        /* only pay the syscall if someone waits */
}

/* mem != 0 → memory-backed (fmemopen); fd is -1 then. mpos/msize track the
 * caller's buffer, and ungot is that path's one-char pushback. fd-backed FILEs
 * leave mem zero and push back into the UNGET slack below buf, like musl.
 * buf_size == 0 means unbuffered (_IONBF / stderr); buf itself stays valid so
 * ungetc still has its slack. lbf == '\n' means line-buffered, -1 otherwise. */
struct _CC9_FILE {
	int fd; int ungot; int eof; int err;
	char *mem; size_t mpos; size_t msize;
	unsigned char *rpos, *rend;              /* read window: unread bytes are [rpos,rend) */
	unsigned char *wpos, *wbase, *wend;      /* write window: pending bytes are [wbase,wpos) */
	unsigned char *buf; size_t buf_size;
	int lbf;
	struct lk lk;
	struct _CC9_FILE *next;                  /* open-file list, for fflush(0) */
};
typedef struct _CC9_FILE FILE;

static unsigned char stdin_buf[UNGET + CC9_BUFSIZ], stdout_buf[UNGET + CC9_BUFSIZ], stderr_buf[UNGET];
static FILE _stdin, _stdout, _stderr;   /* tentative: the initializers below cross-reference */
/* stdout starts LINE buffered and downgrades to full at its first write if it
 * is not the console — same safe default as musl, which probes with an ioctl.
 * stderr is unbuffered per C11 7.21.3p7, and it must stay that way for a
 * second reason: crt0's own diagnostics pwrite fd 2 directly, so a buffer here
 * would reorder them against the program's. */
static FILE _stdin  = { .fd = 0, .ungot = -1, .lbf = -1,   .buf = stdin_buf  + UNGET, .buf_size = CC9_BUFSIZ, .next = &_stdout };
static FILE _stdout = { .fd = 1, .ungot = -1, .lbf = '\n', .buf = stdout_buf + UNGET, .buf_size = CC9_BUFSIZ, .next = &_stderr };
static FILE _stderr = { .fd = 2, .ungot = -1, .lbf = -1,   .buf = stderr_buf + UNGET, .buf_size = 0 };
FILE *stdin = &_stdin, *stdout = &_stdout, *stderr = &_stderr;

/* Open-file list for fflush(0)/exit. Lock order is ALWAYS ofl_lk before a
 * FILE's own lock (fclose unlinks under ofl_lk, then flushes) — the reverse
 * anywhere would deadlock against fflush(0)'s walk. */
static struct lk ofl_lk;
static FILE *ofl_head = &_stdin;
static void ofl_push(FILE *f){ lock(&ofl_lk); f->next = ofl_head; ofl_head = f; unlock(&ofl_lk); }
static void ofl_unlink(FILE *f){   /* caller holds ofl_lk */
	for(FILE **p = &ofl_head; *p; p = &(*p)->next)
		if(*p == f){ *p = f->next; return; }
}

/* ---- fmemopen path: no interposed buffer, the caller's array is the buffer ---- */
static size_t mem_write(FILE *f, const unsigned char *s, size_t l){
	size_t avail = f->msize - f->mpos, want = l < avail ? l : avail;
	memcpy(f->mem + f->mpos, s, want); f->mpos += want;
	return want;
}
static int mem_getc(FILE *f){
	if(f->ungot >= 0){ int u = f->ungot; f->ungot = -1; return u; }
	if(f->mpos >= f->msize){ f->eof = 1; return -1; }
	return (unsigned char)f->mem[f->mpos++];
}

/* ---- buffered core (musl) ---- */
static int stdout_lbf_done;   /* one-shot; setvbuf on stdout claims it too */
/* musl's __stdout_write decides stdout's buffering at the first write (ioctl
 * TIOCGWINSZ, drop to full buffering if it fails). Plan 9 has no ioctl: ask
 * fd2path whether the fd is the console. If that can't answer, STAY line
 * buffered — that is the safe direction, a full-buffered tty swallows prompts. */
static void resolve_stdout_lbf(FILE *f){
	if(f != &_stdout || stdout_lbf_done) return;
	stdout_lbf_done = 1;
	char p[64];
	if(n9_fd2path(f->fd, p, sizeof p) >= 0 && strcmp(p, "/dev/cons") != 0) f->lbf = -1;
}

int fflush(FILE *);
static int atexit_armed;
static void stdio_atexit(void){ fflush(0); }

/* one pwrite can short-write on pipes/devices/9P mounts; keep going until
 * everything is written or an error, else the tail is silently lost. */
static int write_all(FILE *f, const unsigned char *s, size_t n, size_t *done){
	size_t d = 0;
	while(d < n){
		long w = n9_pwrite(f->fd, s + d, (long)(n - d), -1);
		if(w <= 0) break;
		d += (size_t)w;
	}
	*done = d;
	return d == n;
}

/* musl __stdio_write, minus the writev. Returns len on success. On failure it
 * leaves wpos == 0; fflush/fseek test exactly that to detect the error. */
static size_t stdio_write(FILE *f, const unsigned char *s, size_t len){
	size_t d;
	if(f->wpos != f->wbase && !write_all(f, f->wbase, (size_t)(f->wpos - f->wbase), &d)){
		f->wpos = f->wbase = f->wend = 0; f->err = 1; return 0;
	}
	if(len && !write_all(f, s, len, &d)){
		f->wpos = f->wbase = f->wend = 0; f->err = 1; return d;
	}
	f->wend = f->buf + f->buf_size; f->wpos = f->wbase = f->buf;
	return len;
}

/* musl __stdio_read, minus the readv (see the divergence note up top). */
static size_t stdio_read(FILE *f, unsigned char *dest, size_t len){
	long cnt;
	if(len >= f->buf_size){   /* big enough to not be worth a bounce copy (and the buf_size==0 case) */
		cnt = n9_pread(f->fd, dest, (long)len, -1);
		if(cnt <= 0){ if(cnt < 0) f->err = 1; else f->eof = 1; return 0; }
		return (size_t)cnt;
	}
	cnt = n9_pread(f->fd, f->buf, (long)f->buf_size, -1);
	if(cnt <= 0){ if(cnt < 0) f->err = 1; else f->eof = 1; return 0; }
	f->rpos = f->buf; f->rend = f->buf + (size_t)cnt;
	if((size_t)cnt < len) len = (size_t)cnt;
	memcpy(dest, f->rpos, len); f->rpos += len;
	return len;
}

static int toread(FILE *f){   /* musl __toread */
	if(f->wpos != f->wbase) stdio_write(f, 0, 0);   /* write→read: flush first */
	f->wpos = f->wbase = f->wend = 0;
	/* empty, positioned at the far end so ungetc gets the whole buffer as slack */
	f->rpos = f->rend = f->buf + f->buf_size;
	return f->eof ? -1 : 0;
}
static int towrite(FILE *f){   /* musl __towrite */
	/* Leaving read mode. musl just drops the read buffer (C11 7.21.5.3p7 makes
	 * read→write without an intervening seek undefined). We hand the unread
	 * bytes back to the fd first so the write lands where the reader logically
	 * stopped — the old unbuffered layer had no gap here and it costs one seek
	 * on a path that is rare. Unseekable fds (pipes) just fail the seek. */
	if(f->rend && f->rpos != f->rend){ long long r; n9_seek(&r, f->fd, (long long)(f->rpos - f->rend), 1); }
	f->rpos = f->rend = 0;
	f->wpos = f->wbase = f->buf;
	f->wend = f->buf + f->buf_size;
	resolve_stdout_lbf(f);
	/* Arm the exit flush the first time anything can be left pending. musl does
	 * this with a weak-symbol hook; a lazy atexit is the same idea and cannot be
	 * dropped by section GC the way a constructor can. */
	if(!atexit_armed){ atexit_armed = 1; atexit(stdio_atexit); }
	return 0;
}

static int uflow(FILE *f){   /* musl __uflow */
	unsigned char c;
	if(f->mem) return mem_getc(f);
	if(!toread(f) && stdio_read(f, &c, 1) == 1) return c;
	return -1;
}
static int overflow(FILE *f, int _c){   /* musl __overflow */
	unsigned char c = (unsigned char)_c;
	if(f->mem) return mem_write(f, &c, 1) == 1 ? (int)c : -1;
	if(!f->wend && towrite(f)) return -1;
	if(f->wpos != f->wend && (int)c != f->lbf) return *f->wpos++ = c;
	/* Here musl writev()s [pending buffer, c] as one vector. Without writev the
	 * literal port costs TWO syscalls per line on a line-buffered stream and
	 * splits every line into buffer+newline on the wire (visible on a pipe or
	 * the console). So: stage c into the buffer and flush once instead. Same
	 * bytes, one syscall, the line stays one write. */
	if(f->buf_size && f->wpos == f->wend){   /* buffer full: drain, then c fits */
		stdio_write(f, 0, 0);
		if(!f->wpos) return -1;
	}
	if(f->wpos != f->wend){
		*f->wpos++ = c;
		if((int)c != f->lbf) return c;
		stdio_write(f, 0, 0);
		return f->wpos ? (int)c : -1;
	}
	/* unbuffered (buf_size == 0): straight out, there is nowhere to stage it */
	if(stdio_write(f, &c, 1) != 1) return -1;
	return c;
}
/* musl's getc_unlocked/putc_unlocked. Memory streams keep rpos==rend and
 * wpos==wend==0, so they fall through to uflow/overflow and the mem path. */
static int getc_(FILE *f){ return f->rpos != f->rend ? *f->rpos++ : uflow(f); }
static int putc_(int c, FILE *f){
	return ((unsigned char)c != f->lbf && f->wpos != f->wend)
		? (*f->wpos++ = (unsigned char)c)
		: overflow(f, c);
}

static size_t fwritex(const unsigned char *s, size_t l, FILE *f){   /* musl __fwritex */
	size_t i = 0;
	if(f->mem) return mem_write(f, s, l);
	if(!f->wend && towrite(f)) return 0;
	if(l > (size_t)(f->wend - f->wpos)) return stdio_write(f, s, l);
	if(f->lbf >= 0){
		for(i = l; i && s[i-1] != '\n'; i--);   /* match /^(.*\n|)/ — flush through the last newline */
		if(i){
			size_t n = stdio_write(f, s, i);
			if(n < i) return n;
			s += i; l -= i;
		}
	}
	memcpy(f->wpos, s, l); f->wpos += l;
	return l + i;
}

static int flush_(FILE *f){   /* musl fflush's per-stream body */
	if(f->wpos != f->wbase){
		stdio_write(f, 0, 0);
		if(!f->wpos) return -1;
	}
	/* reading: give the fd back the bytes we read ahead but never handed out
	 * (POSIX requires the position to be synced). */
	if(f->rpos != f->rend){ long long r; n9_seek(&r, f->fd, (long long)(f->rpos - f->rend), 1); }
	f->wpos = f->wbase = f->wend = 0;
	f->rpos = f->rend = 0;
	return 0;
}

/* ---- public surface ---- */
size_t fwrite(const void *p, size_t sz, size_t n, FILE *f){
	if(!sz || !n) return 0;
	size_t l = sz * n, k;
	lock(&f->lk); k = fwritex(p, l, f); unlock(&f->lk);
	return k == l ? n : k / sz;
}
size_t fread(void *p, size_t sz, size_t n, FILE *f){
	if(!sz || !n) return 0;
	unsigned char *d = p; size_t len = sz * n, l = len, k;
	lock(&f->lk);
	if(f->mem){   /* memory-backed: read from the caller's buffer */
		size_t avail = f->msize - f->mpos, want = l < avail ? l : avail;
		if(want < l) f->eof = 1;
		memcpy(d, f->mem + f->mpos, want); f->mpos += want;
		unlock(&f->lk);
		return want / sz;
	}
	if(f->rpos != f->rend){        /* first exhaust the buffer */
		k = (size_t)(f->rend - f->rpos); if(k > l) k = l;
		memcpy(d, f->rpos, k); f->rpos += k; d += k; l -= k;
	}
	for(; l; l -= k, d += k){      /* then read the remainder (big reads go straight through) */
		k = toread(f) ? 0 : stdio_read(f, d, l);
		if(!k){ unlock(&f->lk); return (len - l) / sz; }
	}
	unlock(&f->lk);
	return n;
}
int fputc(int c, FILE *f){ lock(&f->lk); int r = putc_(c, f); unlock(&f->lk); return r; }
int putc(int c, FILE *f){ return fputc(c,f); }
int putchar(int c){ return fputc(c, stdout); }
int fgetc(FILE *f){ lock(&f->lk); int r = getc_(f); unlock(&f->lk); return r; }
int getc(FILE *f){ return fgetc(f); }
int getchar(void){ return fgetc(stdin); }
int ungetc(int c, FILE *f){
	if(c == -1) return -1;
	lock(&f->lk);
	if(f->mem){ f->ungot = (unsigned char)c; f->eof = 0; unlock(&f->lk); return (unsigned char)c; }
	if(!f->rpos) toread(f);
	if(!f->rpos || f->rpos <= f->buf - UNGET){ unlock(&f->lk); return -1; }   /* out of slack (C guarantees one pushback; musl gives 8) */
	*--f->rpos = (unsigned char)c;
	f->eof = 0;
	unlock(&f->lk);
	return (unsigned char)c;
}
int fputs(const char *s, FILE *f){
	size_t n = 0; while(s[n]) n++;
	lock(&f->lk); size_t k = fwritex((const unsigned char *)s, n, f); unlock(&f->lk);
	return k == n ? 0 : -1;
}
int puts(const char *s){
	size_t n = 0; while(s[n]) n++;
	FILE *f = stdout;
	lock(&f->lk);
	int r = (fwritex((const unsigned char *)s, n, f) == n &&
	         fwritex((const unsigned char *)"\n", 1, f) == 1) ? '\n' : -1;
	unlock(&f->lk);
	return r;
}
char *fgets(char *s, int n, FILE *f){
	int i = 0;
	lock(&f->lk);
	while(i < n-1){
		int c = getc_(f);
		if(c == -1){ if(i == 0){ unlock(&f->lk); return 0; } break; }
		s[i++] = (char)c;
		if(c == '\n') break;
	}
	unlock(&f->lk);
	s[i] = 0; return s;
}
int fflush(FILE *f){
	if(!f){   /* flush every open stream (musl fflush(NULL)) */
		int r = 0;
		lock(&ofl_lk);
		for(FILE *p = ofl_head; p; p = p->next){
			lock(&p->lk);
			if(p->wpos != p->wbase) r |= flush_(p);
			unlock(&p->lk);
		}
		unlock(&ofl_lk);
		return r;
	}
	lock(&f->lk); int r = flush_(f); unlock(&f->lk);
	return r;
}
/* Flush at exit, belt AND braces — an unflushed stdout silently loses output,
 * the one failure this layer must not have:
 *  - .fini_array: crt0 runs it AFTER every atexit/__cxa_atexit handler, so
 *    output from a static object's destructor is still caught. Primary.
 *  - the lazy atexit() armed in towrite(): backstop if .fini_array is ever
 *    dropped. It runs earlier in the LIFO chain, so it can miss a static
 *    destructor's output — hence the destructor is the one that matters.
 * flush_() is idempotent, so running both costs one no-op pass. */
__attribute__((destructor)) static void stdio_fini(void){ fflush(0); }
int feof(FILE *f){ return f->eof; }
int ferror(FILE *f){ return f->err; }
void clearerr(FILE *f){ f->eof = f->err = 0; }
/* perror: write "s: <errno message>\n" to stderr (libc++ test framework uses it). */
extern int *__n9_errno(void); extern char *strerror(int);
int fprintf(FILE *, const char *, ...);
void perror(const char *s){ if(s && *s) fprintf(stderr, "%s: ", s); fprintf(stderr, "%s\n", strerror(*__n9_errno())); }
int fileno(FILE *f){ return f->fd; }

/* printf family over vsnprintf (n9libc) + fwrite. */
extern int vsnprintf(char *, size_t, const char *, __builtin_va_list);
int vfprintf(FILE *f, const char *fmt, __builtin_va_list ap){
	char buf[1024];
	__builtin_va_list ap2; __builtin_va_copy(ap2, ap);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	if(n < 0){ __builtin_va_end(ap2); return n; }
	if(n < (int)sizeof buf){ __builtin_va_end(ap2); return (int)fwrite(buf, 1, (size_t)n, f); }
	/* didn't fit: reformat into an exact-size heap buffer so nothing is dropped. */
	char *big = malloc((size_t)n + 1);
	if(!big){ __builtin_va_end(ap2); return (int)fwrite(buf, 1, sizeof buf - 1, f); }   /* fall back */
	int n2 = vsnprintf(big, (size_t)n + 1, fmt, ap2);
	__builtin_va_end(ap2);
	int w = (int)fwrite(big, 1, (size_t)(n2 < 0 ? 0 : n2), f);
	free(big);
	return w;
}
int fprintf(FILE *f, const char *fmt, ...){ __builtin_va_list ap; __builtin_va_start(ap,fmt); int r=vfprintf(f,fmt,ap); __builtin_va_end(ap); return r; }
int printf(const char *fmt, ...){ __builtin_va_list ap; __builtin_va_start(ap,fmt); int r=vfprintf(stdout,fmt,ap); __builtin_va_end(ap); return r; }
int vprintf(const char *fmt, __builtin_va_list ap){ return vfprintf(stdout, fmt, ap); }
/* sscanf/vsscanf live in printf.c (real implementation). */

/* real files over Plan 9 (backs std::fstream / basic_filebuf). */
extern long n9_open(const char *, int);
extern long n9_create(const char *, int, unsigned long);
extern long n9_close(int);
extern long n9_stat(const char *, unsigned char *, int);
/* One allocation for the FILE and its buffer (musl does the same): buf is then
 * never NULL for an fd-backed stream, which stdio_write's success test relies
 * on, and fclose has nothing extra to free. */
static FILE *newfile(int fd){
	FILE *f = malloc(sizeof *f + UNGET + CC9_BUFSIZ);
	if(!f) return 0;
	memset(f, 0, sizeof *f);
	f->fd = fd; f->ungot = -1; f->lbf = -1;
	f->buf = (unsigned char *)(f + 1) + UNGET;
	f->buf_size = CC9_BUFSIZ;
	ofl_push(f);
	return f;
}
FILE *fopen(const char *path, const char *mode){
	int omode=0, creat=0, trunc=0, append=0, plus=0, excl=0;
	for(const char *m=mode; *m; m++){ if(*m=='+') plus=1; else if(*m=='x') excl=1; }
	if(mode[0]=='r') omode = plus?2:0;
	else if(mode[0]=='w'){ omode = plus?2:1; creat=1; trunc=1; }
	else if(mode[0]=='a'){ omode = plus?2:1; creat=1; append=1; }
	/* C11/C++23 'x': exclusive create — fail if the path already exists. Plan 9
	 * create() always truncates, so guard with a stat (matches fs.c open O_EXCL). */
	if(excl){ unsigned char sb[512]; if(n9_stat(path, sb, sizeof sb) >= 0) return 0; }
	long fd = creat ? n9_create(path, omode|(trunc?16:0), 0666) : n9_open(path, omode);
	if(fd<0){   /* propagate a real errno (getpath.py's readlines needs ENOENT) */
		extern int cc9_errno_from_errstr(void);
		extern int *__n9_errno(void);
		*__n9_errno() = cc9_errno_from_errstr();
		return 0;
	}
	FILE *f = newfile((int)fd); if(!f){ n9_close((int)fd); return 0; }
	if(append){ long long r; n9_seek(&r,(int)fd,0,2); }
	return f;
}
FILE *fdopen(int fd, const char *mode){ (void)mode; return newfile(fd); }
FILE *freopen(const char *path, const char *mode, FILE *f){
	if(!f) return 0;
	fflush(f);
	if(f->fd>2) n9_close(f->fd);
	FILE *nf = fopen(path,mode); if(!nf) return 0;
	/* Adopt nf's fd but keep f's own buffer; nf (and the buffer in its
	 * allocation) is unlinked and freed. */
	lock(&ofl_lk); ofl_unlink(nf); unlock(&ofl_lk);
	f->fd = nf->fd; free(nf);
	f->ungot=-1; f->eof=0; f->err=0; f->mem=0; f->mpos=0; f->msize=0;
	f->rpos=f->rend=0; f->wpos=f->wbase=f->wend=0;
	return f;
}
/* fmemopen: a FILE* backed by the caller's buffer. ponytail: buf must be
 * non-null and modes start at pos 0 (no 'a'/null-buf alloc) — all the tests need.
 * No stdio buffer is allocated: the caller's array is the buffer. */
FILE *fmemopen(void *buf, size_t size, const char *mode){
	(void)mode; if(!buf) return 0;
	FILE *f=malloc(sizeof *f); if(!f) return 0;
	memset(f, 0, sizeof *f);
	f->fd=-1; f->ungot=-1; f->lbf=-1; f->mem=buf; f->msize=size;
	ofl_push(f);
	return f;
}
int fclose(FILE *f){
	if(!f) return 0;
	int std = (f==stdin || f==stdout || f==stderr);
	lock(&ofl_lk); if(!std) ofl_unlink(f); unlock(&ofl_lk);
	lock(&f->lk); int r = flush_(f); unlock(&f->lk);
	if(std) return r;                          /* std streams: flush, never close (as before) */
	int fd = f->fd, mem = (f->mem != 0);
	free(f);
	if(mem) return r;
	return (n9_close(fd) < 0 || r) ? -1 : 0;
}
long ftell(FILE *f){
	if(f->mem) return (long)f->mpos;
	lock(&f->lk);
	long long r = 0;
	if(n9_seek(&r,f->fd,0,1) < 0){ unlock(&f->lk); return -1; }
	/* the fd's offset is not the stream's: adjust for what sits in the buffer
	 * (musl __ftello_unlocked). */
	if(f->rend) r += f->rpos - f->rend;
	else if(f->wbase) r += f->wpos - f->wbase;
	unlock(&f->lk);
	return (long)r;
}
long ftello(FILE *f){ return ftell(f); }
int fseek(FILE *f, long off, int whence){   /* musl __fseeko_unlocked */
	if(whence != 0 && whence != 1 && whence != 2) return -1;
	lock(&f->lk);
	f->ungot = -1; f->eof = 0;
	if(f->mem){ size_t base = whence==1 ? f->mpos : whence==2 ? f->msize : 0; f->mpos = base + (size_t)off; unlock(&f->lk); return 0; }
	if(whence == 1 && f->rend) off -= f->rend - f->rpos;   /* relative to the stream, not the fd */
	if(f->wpos != f->wbase){ stdio_write(f, 0, 0); if(!f->wpos){ unlock(&f->lk); return -1; } }
	f->wpos = f->wbase = f->wend = 0;
	long long r;
	if(n9_seek(&r,f->fd,off,whence) < 0){ unlock(&f->lk); return -1; }
	f->rpos = f->rend = 0;   /* the seek succeeded, so the read buffer is stale */
	unlock(&f->lk);
	return 0;
}
int fseeko(FILE *f, long off, int whence){ return fseek(f,off,whence); }
void rewind(FILE *f){ fseek(f,0,0); }
int fgetpos(FILE *f, void *pos){ long *p=pos; *p=ftell(f); return 0; }
int fsetpos(FILE *f, const void *pos){ const long *p=pos; return fseek(f,*p,0); }
/* setvbuf/setbuf (musl src/stdio/setvbuf.c). These used to take a buffer and a
 * mode, throw both away and return 0 — a lie that hid the missing buffering for
 * everyone who configured their streams properly (Python does, at startup).
 * musl doesn't lock or validate here because the call is undefined unless it is
 * the first operation on the stream; we reject that case instead of quietly
 * corrupting a stream mid-flight, which is what the spec allows and what any
 * caller checking the return value expects. */
int setvbuf(FILE *f, char *b, int type, size_t size){
	lock(&f->lk);
	if(f->rpos || f->wbase || f->mem){ unlock(&f->lk); return -1; }   /* I/O already begun */
	f->lbf = -1;
	if(type == _IONBF){
		f->buf_size = 0;   /* buf stays valid: ungetc still needs its slack */
	} else if(type == _IOLBF || type == _IOFBF){
		if(b && size >= UNGET){ f->buf = (unsigned char *)b + UNGET; f->buf_size = size - UNGET; }
		if(type == _IOLBF && f->buf_size) f->lbf = '\n';
	} else {
		unlock(&f->lk); return -1;
	}
	if(f == &_stdout) stdout_lbf_done = 1;   /* the caller's choice beats the tty probe (musl's F_SVB) */
	unlock(&f->lk);
	return 0;
}
void setbuf(FILE *f, char *b){ setvbuf(f, b, b ? _IOFBF : _IONBF, CC9_BUFSIZ); }

/* tmpnam/tmpfile — /tmp/cc9tmp.<pid>.<seq>. ponytail: tmpfile() files are not
 * auto-removed at close (Plan 9 ORCLOSE isn't plumbed through fopen); /tmp is
 * per-boot on 9front, so leaks are bounded. Plumb ORCLOSE if it ever matters. */
extern int getpid(void);
extern int snprintf(char *, size_t, const char *, ...);
static int cc9_tmpseq;
char *tmpnam(char *buf){
	static char sbuf[64];
	char *p = buf ? buf : sbuf;
	snprintf(p, 64, "/tmp/cc9tmp.%d.%d", getpid(), ++cc9_tmpseq);
	return p;
}
FILE *tmpfile(void){
	char name[64];
	snprintf(name, sizeof name, "/tmp/cc9tmp.%d.%d", getpid(), ++cc9_tmpseq);
	return fopen(name, "w+");
}

/* fscanf — only "%lf", which backs LuaJIT/Lua io.read("*n"). Skips leading
 * whitespace, gathers one number token (dec/hex float chars), pushes back the
 * first non-member byte, strtod()s the token. Extend on contact. */
extern double strtod(const char *, char **);
static int cc9_numchar(int c, int i){
	if((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')||c=='.'||c=='x'||c=='X') return 1;
	if((c=='+'||c=='-') && i==0) return 1;
	if(c=='p'||c=='P') return 1;
	return 0;
}
int fscanf(FILE *f, const char *fmt, ...){
	if(!(fmt[0]=='%'&&fmt[1]=='l'&&fmt[2]=='f'&&fmt[3]==0)) return -1;
	int c;
	do { c = fgetc(f); } while(c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v');
	char tok[64]; int n = 0;
	/* exponent sign: allow +/- right after e/E/p/P too */
	while(c!=-1 && n < (int)sizeof tok - 1 &&
	      (cc9_numchar(c, n) ||
	       ((c=='+'||c=='-') && n>0 && (tok[n-1]=='e'||tok[n-1]=='E'||tok[n-1]=='p'||tok[n-1]=='P')))){
		tok[n++] = (char)c;
		c = fgetc(f);
	}
	if(c!=-1) ungetc(c, f);
	tok[n] = 0;
	if(n==0) return f->eof ? -1 : 0;
	char *end = tok;
	__builtin_va_list ap; __builtin_va_start(ap, fmt);
	double *out = __builtin_va_arg(ap, double *);
	__builtin_va_end(ap);
	double v = strtod(tok, &end);
	if(end == tok) return 0;
	*out = v;
	return 1;
}
