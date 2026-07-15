/* cc9 filesystem layer — POSIX file API over Plan 9 syscalls (a small slice of
 * APE). Plan 9 has no POSIX stat: STAT/FSTAT fill a wire-format Dir (stat(5));
 * we parse the fields we need. Directory reads return concatenated Dir entries.
 * POSIX O_RDONLY/WRONLY/RDWR (0/1/2) happen to equal Plan 9 OREAD/OWRITE/ORDWR. */
typedef unsigned long size_t;
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>

extern long n9_open(const char *, int);
extern long n9_create(const char *, int, unsigned long);
extern long n9_close(int);
extern long n9_pread(int, void *, long, long long);
extern long n9_pwrite(int, const void *, long, long long);
extern long n9_seek(long long *, int, long long, int);
extern long n9_stat(const char *, unsigned char *, int);
extern long n9_fstat(int, unsigned char *, int);
extern long n9_remove(const char *);
extern long n9_chdir(const char *);
extern long n9_fd2path(int, char *, int);
extern void *malloc(size_t); extern void free(void *);
extern void *memset(void *, int, size_t);
extern size_t strlen(const char *);

/* Defined below; declared here because open() (above them) must map its errno
 * from the kernel's error string rather than guess. */
int cc9_errno_from_errstr(void);
int cc9_errno_from_errstr_or(int dflt);

#define OTRUNC 16
#define DMDIR  0x80000000UL

static unsigned long long le(const unsigned char *p, int n){
	unsigned long long v=0; for(int i=0;i<n;i++) v |= (unsigned long long)p[i]<<(8*i); return v;
}
/* Fill struct stat from a wire Dir (stat(5)): mode@21 atime@25 mtime@29
 * length@33 qid.path@13. Little-endian. */
static void dir_to_stat(const unsigned char *b, struct stat *st){
	memset(st, 0, sizeof *st);
	unsigned long mode = (unsigned long)le(b+21,4);
	/* Dir.type (the serving device's character, wire offset 2): '|' = pipe.
	 * Reporting pipes as S_IFIFO is load-bearing — libuv's uv_guess_handle
	 * classifies fds by fstat, and a pipe reported as S_IFREG sends stream
	 * users (nvim RPC channels) down the blocking-file path, which wedges. */
	unsigned dtype = (unsigned)le(b+2,2);
	st->st_ino  = le(b+13,8);
	st->st_size = (long)le(b+33,8);
	if(dtype == '|')
		st->st_mode = S_IFIFO | (mode & 0777);
	else
		st->st_mode = ((mode & DMDIR) ? S_IFDIR : S_IFREG) | (mode & 0777);
	st->st_nlink = 1;
	st->st_atim.tv_sec = (long)le(b+25,4);
	st->st_mtim.tv_sec = (long)le(b+29,4);
	st->st_ctim.tv_sec = st->st_mtim.tv_sec;
	st->st_blksize = 8192;
	st->st_blocks = (st->st_size + 511) / 512;
}

static int streq(const char *a, const char *b){ while(*a&&*a==*b){a++;b++;} return *a==*b; }

/* Resolve an *at() (dfd, path) pair to a plain path. Plan 9 has no openat
 * family, so a relative path is rebased onto the parent fd's path (fd2path);
 * absolute paths and AT_FDCWD pass through. This is what makes libc++'s
 * fd-threaded remove_all recurse into the right subdirectories. */
static const char *at_path(int dfd, const char *path, char *buf, int bufsz){
	if(!path || path[0]=='/' || dfd==AT_FDCWD) return path;
	if(n9_fd2path(dfd, buf, bufsz) < 0) return path;   /* fall back; caller will error */
	int i=0; while(buf[i] && i<bufsz-2) i++;
	if(i==0 || buf[i-1]!='/') buf[i++]='/';
	int j=0; while(path[j] && i<bufsz-1) buf[i++]=path[j++];
	buf[i]=0; return buf;
}

int open(const char *path, int flags, ...){
	if(streq(path,"/dev/urandom")) path="/dev/random";   /* Plan 9 entropy source */
	int omode = flags & O_ACCMODE;        /* OREAD/OWRITE/ORDWR == 0/1/2 */
	if(flags & O_TRUNC) omode |= OTRUNC;
	unsigned long perm = (flags & O_DIRECTORY) ? (DMDIR|0777) : 0666;
	long fd;
	if(flags & O_CREAT){
		/* Plan 9 create() ALWAYS truncates an existing file, so using it for a
		 * bare O_CREAT would silently destroy data. Only create() when the file
		 * must be made/truncated; otherwise open the existing file in place. */
		if(flags & O_EXCL){
			struct stat st; if(stat(path,&st)==0){ errno=EEXIST; return -1; }   /* must not exist */
			fd = n9_create(path, omode, perm);
		} else if(flags & O_TRUNC){
			fd = n9_create(path, omode, perm);
		} else {
			fd = n9_open(path, omode);                 /* keep existing contents */
			if(fd < 0) fd = n9_create(path, omode, perm);   /* absent -> create */
		}
	} else fd = n9_open(path, omode);
	/* Map the real error rather than assert one. This used to be
	 * `errno = (flags & O_CREAT) ? EACCES : ENOENT`, i.e. EVERY plain open that
	 * failed claimed the file did not exist — a permission denial, a non-directory
	 * component, or opening a directory for writing all came back ENOENT, and the
	 * errstr was never stashed so the caller could not even see the real message.
	 * Callers branch on this (std::fs, libc++ filesystem), so it has to be true. */
	if(fd < 0){ errno = cc9_errno_from_errstr_or((flags & O_CREAT) ? EACCES : ENOENT); return -1; }
	return (int)fd;
}
int openat(int dfd, const char *path, int flags, ...){ char b[1024]; return open(at_path(dfd,path,b,sizeof b), flags); }
/* poll.c readiness layer (libuv): fds with a reader thread or O_NONBLOCK are
 * served from its ring buffer; everything else takes the direct path. */
extern int cc9_poll_owned(int);
extern long cc9_poll_read(int, void *, long);
extern void cc9_poll_onclose(int);
extern void cc9_net_onclose(int);
int close(int fd){ cc9_poll_onclose(fd); cc9_net_onclose(fd); return n9_close(fd) < 0 ? -1 : 0; }
/* EIO is the FALLBACK, not the answer. These carry socket traffic (net9.c reads
 * and writes /net data fds through them), where the difference between a peer
 * hangup (ECONNRESET) and a generic I/O error decides whether the caller retries,
 * reports a clean disconnect, or gives up — and "interrupted" must surface as
 * EINTR so retry loops actually retry. */
long read(int fd, void *buf, size_t n){
	if(cc9_poll_owned(fd)) return cc9_poll_read(fd, buf, (long)n);
	long r=n9_pread(fd,buf,(long)n,-1); if(r<0)errno=cc9_errno_from_errstr_or(EIO); return r; }
extern void cc9_trace(const char *, int, long);
extern int cc9_poll_wowned(int);
extern long cc9_poll_write(int, const void *, long);
long write(int fd, const void *buf, size_t n){
	if(cc9_poll_wowned(fd)) return cc9_poll_write(fd, buf, (long)n);  /* O_NONBLOCK: ring + honest POLLOUT */
	long r=n9_pwrite(fd,buf,(long)n,-1); cc9_trace("write", fd, r); if(r<0)errno=cc9_errno_from_errstr_or(EIO); return r; }
long pread(int fd, void *buf, size_t n, long off){ return n9_pread(fd,buf,(long)n,off); }
long pwrite(int fd, const void *buf, size_t n, long off){ return n9_pwrite(fd,buf,(long)n,off); }
long lseek(int fd, long off, int whence){ long long r=0; if(n9_seek(&r,fd,off,whence)<0){errno=EIO;return -1;} return (long)r; }

extern long n9_errstr(char *, int);
static int cc9_contains(const char *h, const char *n){
	for(; *h; h++){ const char *a=h,*b=n; while(*b && *a==*b){a++;b++;} if(!*b) return 1; }
	return 0;
}
/* Map the Plan 9 last-error string (errors are text on Plan 9) to a POSIX errno.
 * Substring-matches the common kernel messages; falls back to ENOENT for the
 * unrecognized (the historical cc9 default). std::filesystem error_code checks
 * (e.g. temp_directory_path expecting permission_denied) depend on this.
 * ponytail: substring table, inherently lossy — Plan 9 has no errno, so the
 * mapping can only ever be best-effort. Upgrade path is to add cases as new
 * kernel strings surface (not to restructure); the raw errstr is the source of
 * truth and could instead be threaded into std::system_error::what() verbatim. */
/* Last kernel errstr + the errno it mapped to, for io::Error message fidelity
 * (Rust's error_string shows the real Plan 9 string instead of "os error N").
 *
 * PER-THREAD, exactly like errno, via pthread.c's weak stack-range slot (a
 * thread-free link leaves the symbol unsatisfied and uses the global below).
 * It was process-global, on the theory that "a caller compares the stashed errno
 * to the one it holds so a stale string can't mislabel". That does not hold: two
 * unrelated failures on two threads share an errno all the time (ENOENT), the
 * comparison passes, and the caller prints another thread's error text — Servo
 * reported a missing font dir as "file does not exist: '.../reg.sqlite-wal'". */
extern char *cc9_thread_errstr_slot(int **, int *) __attribute__((weak));
static char cc9_global_errstr[160];
static int cc9_global_errstr_errno = 0;
/* Returns the calling thread's stash and, via cap_out, how big it is — the
 * per-thread buffer is defined in pthread.c, so its size must travel with the
 * pointer rather than be repeated here. */
static char *cc9_errstr_buf(int **eno_out, int *cap_out){
	if(cc9_thread_errstr_slot){
		int *e = 0, cap = 0; char *b = cc9_thread_errstr_slot(&e, &cap);
		if(b && cap > 0){ if(eno_out) *eno_out = e; *cap_out = cap; return b; }
	}
	if(eno_out) *eno_out = &cc9_global_errstr_errno;
	*cap_out = (int)sizeof cc9_global_errstr;
	return cc9_global_errstr;
}
const char *__n9_errstr_last(int *eno){
	int *slot_eno, cap;
	const char *buf = cc9_errstr_buf(&slot_eno, &cap);
	if(eno) *eno = *slot_eno;
	return buf;
}
/* `dflt` is what an unrecognised message maps to. It is a parameter because the
 * right guess depends on the call: a failed open of an unknown-shaped error is
 * most likely ENOENT, but a failed read/write is not — reporting a dead socket as
 * "file not found" sends the caller somewhere useless. */
int cc9_errno_from_errstr_or(int dflt){
	/* errstr fills the buffer (kernel NUL-terminates) but its return value is not
	 * the length — read the buffer directly. */
	char e[160]; e[0]=0; e[sizeof e-1]=0;
	n9_errstr(e, sizeof e - 1);
	int *stash_eno, cap;
	char *stash = cc9_errstr_buf(&stash_eno, &cap);
	int i = 0;
	for(; i < cap - 1 && e[i]; i++) stash[i] = e[i];
	stash[i] = 0;
	int r = dflt;
	if(e[0] == 0) ;
	else if(cc9_contains(e, "permission")) r = EACCES;
	else if(cc9_contains(e, "exists"))     r = EEXIST;
	else if(cc9_contains(e, "not empty"))  r = ENOTEMPTY;
	else if(cc9_contains(e, "not a directory") || cc9_contains(e, "not a dir")) r = ENOTDIR;
	else if(cc9_contains(e, "is a directory")) r = EISDIR;
	else if(cc9_contains(e, "i/o error"))  r = EIO;
	else if(cc9_contains(e, "refused"))    r = ECONNREFUSED;
	else if(cc9_contains(e, "connection reset") || cc9_contains(e, "hungup")) r = ECONNRESET;
	else if(cc9_contains(e, "timed out"))  r = ETIMEDOUT;
	else if(cc9_contains(e, "unreachable")) r = EHOSTUNREACH;
	else if(cc9_contains(e, "address in use") || cc9_contains(e, "announce"))  r = EADDRINUSE;
	else if(cc9_contains(e, "interrupted")) r = EINTR;
	*stash_eno = r;
	return r;
}
int cc9_errno_from_errstr(void){ return cc9_errno_from_errstr_or(ENOENT); }
int stat(const char *path, struct stat *st){
	unsigned char b[512]; long n=n9_stat(path,b,sizeof b);
	if(n<0){ errno=cc9_errno_from_errstr(); return -1; } dir_to_stat(b,st); return 0;
}
int fstat(int fd, struct stat *st){
	unsigned char b[512]; long n=n9_fstat(fd,b,sizeof b);
	if(n<0){ errno=EBADF; return -1; } dir_to_stat(b,st); return 0;
}
int lstat(const char *path, struct stat *st){ return stat(path, st); }   /* no symlink follow distinction */
int fstatat(int dfd, const char *path, struct stat *st, int flag){ (void)flag; char b[1024]; return stat(at_path(dfd,path,b,sizeof b), st); }

/* The existence check has to come first. Plan 9's create(2) truncates an existing
 * name rather than failing, and there is no exclusive-create mode: called with
 * DMDIR on an existing regular file it wipes the file's contents and still
 * reports success, so probing with create first would destroy the very file we
 * are about to refuse to overwrite. POSIX wants EEXIST for an existing name
 * anyway, so one stat serves both.
 * ponytail: TOCTOU window between stat and create — Plan 9 gives no O_EXCL to
 * close it, so this is the same race every Plan 9 mkdir carries.
 *
 * Failures otherwise map through errstr like the rest of this file. They used to
 * be reported as EACCES unless the name existed, which quietly breaks every
 * mkdir -p: create_dir_all (Rust) and mkdir -p only build a missing parent when
 * mkdir says ENOENT — told "permission denied", they abandon the whole chain. */
int mkdir(const char *path, mode_t m){
	struct stat st;
	if(stat(path, &st) == 0){ errno = EEXIST; return -1; }
	long fd = n9_create(path, 0/*OREAD*/, DMDIR|(m&0777));
	if(fd < 0){ errno = cc9_errno_from_errstr(); return -1; }
	n9_close((int)fd);
	return 0;
}
int mkdirat(int dfd, const char *path, unsigned int m){ char b[1024]; return mkdir(at_path(dfd,path,b,sizeof b),m); }

extern long n9_wstat(const char *, unsigned char *, int);
extern long n9_fwstat(int, unsigned char *, int);
static void putle(unsigned char *p, unsigned long long v, int n){ for(int i=0;i<n;i++) p[i]=(unsigned char)(v>>(8*i)); }
/* Build a wstat Dir with all fields "don't change" (~0 / empty) except those
 * given: name (rename), mode (chmod), length (truncate), mtime. 0xFFFF.. = leave. */
static int build_wstat(unsigned char *buf, const char *name, unsigned long mode, unsigned long long length, unsigned long mtime){
	unsigned char *p = buf + 2;                 /* size filled last */
	putle(p,0xFFFF,2); p+=2;                     /* type */
	putle(p,0xFFFFFFFF,4); p+=4;                 /* dev */
	*p++ = 0xFF;                                 /* qid.type */
	putle(p,0xFFFFFFFF,4); p+=4;                 /* qid.vers */
	putle(p,~0ULL,8); p+=8;                      /* qid.path */
	putle(p,mode,4); p+=4;                       /* mode */
	putle(p,0xFFFFFFFF,4); p+=4;                 /* atime (not settable via wstat) */
	putle(p,mtime,4); p+=4;                      /* mtime */
	putle(p,length,8); p+=8;                     /* length */
	int nl = name ? (int)strlen(name) : 0;
	putle(p,nl,2); p+=2; for(int i=0;i<nl;i++) *p++=(unsigned char)name[i];   /* name */
	putle(p,0,2); p+=2; putle(p,0,2); p+=2; putle(p,0,2); p+=2;               /* uid,gid,muid */
	int total = (int)(p - buf);
	putle(buf, (unsigned)(total-2), 2);
	return total;
}
int chmod(const char *path, mode_t m){
	struct stat st; unsigned long keep=0; if(stat(path,&st)==0) keep = (st.st_mode&S_IFDIR)?DMDIR:0;
	unsigned char b[128]; int n=build_wstat(b,0,keep|(m&0777),~0ULL,0xFFFFFFFF);
	return n9_wstat(path,b,n)<0?(errno=EACCES,-1):0;
}
int fchmod(int fd, mode_t m){ unsigned char b[128]; int n=build_wstat(b,0,m&0777,~0ULL,0xFFFFFFFF); return n9_fwstat(fd,b,n)<0?-1:0; }
/* Set mtime via wstat (atime is not settable on Plan 9 — callers ignore it). */
int cc9_set_mtime(const char *path, unsigned long secs){
	unsigned char b[128]; int n=build_wstat(b,0,0xFFFFFFFF,~0ULL,secs);
	return n9_wstat(path,b,n)<0?(errno=cc9_errno_from_errstr(),-1):0;
}
int cc9_fset_mtime(int fd, unsigned long secs){
	unsigned char b[128]; int n=build_wstat(b,0,0xFFFFFFFF,~0ULL,secs);
	return n9_fwstat(fd,b,n)<0?(errno=cc9_errno_from_errstr(),-1):0;
}
int fchmodat(int d, const char *p, mode_t m, int f){ (void)f; char b[1024]; return chmod(at_path(d,p,b,sizeof b),m); }
long pathconf(const char *p, int name){ (void)p; return name==4 ? 4096 : 255; }
long fpathconf(int fd, int name){ (void)fd; return name==4 ? 4096 : 255; }
/* Not always ENOENT: Plan 9's remove reports "directory not empty" and
 * "permission denied" too, and callers act on the difference — rmdir(2) is this
 * same call, and remove_dir_all/std::filesystem key their recursion off
 * ENOTEMPTY vs ENOENT. */
int unlink(const char *path){ if(n9_remove(path)<0){ errno=cc9_errno_from_errstr(); return -1; } return 0; }
/* Does this directory hold anything? A directory read yields concatenated Dir
 * entries, so "no bytes" means "no entries". NB: this itself sets errstr, so
 * callers must map their error BEFORE asking. */
static int dir_has_entries(const char *path){
	long fd = n9_open(path, 0 /*OREAD*/);
	if(fd < 0) return 0;
	unsigned char b[256];
	long n = n9_pread((int)fd, b, sizeof b, -1);
	n9_close((int)fd);
	return n > 0;
}

/* rmdir(2), not just unlink: POSIX says a non-empty directory is ENOTEMPTY, and
 * remove_dir_all / std::filesystem::remove_all branch on exactly that to decide
 * whether to recurse or to treat the job as done.
 *
 * The file server's wording cannot be trusted for this — 9front's ramfs answers a
 * non-empty remove with a flat "invalid operation", which the errstr table maps to
 * its ENOENT default, i.e. "already gone". So work it out here instead: if the
 * target is still a directory and still has entries, the removal failed because it
 * is not empty, whatever the server chose to call it. */
int rmdir(const char *path){
	if(n9_remove(path) == 0) return 0;
	int e = cc9_errno_from_errstr();   /* map first: the probe below clobbers errstr */
	if(e != EACCES && e != EPERM){
		struct stat st;
		if(stat(path, &st) == 0 && S_ISDIR(st.st_mode) && dir_has_entries(path))
			e = ENOTEMPTY;
	}
	errno = e;
	return -1;
}
int unlinkat(int dfd, const char *path, int flag){ (void)flag; char b[1024]; return unlink(at_path(dfd,path,b,sizeof b)); }
int remove(const char *path){ return unlink(path); }
int chdir(const char *path){ return n9_chdir(path)<0 ? -1 : 0; }
int access(const char *path, int mode){ (void)mode; struct stat st; return stat(path,&st)==0?0:-1; }
/* Plan 9 has no ttys; the terminal (alacritty9/vts/9term) talks to us over
 * pipes. Heuristic: fds 0-2 count as a tty when $TERM is set — that's exactly
 * the environment a terminal emulator provides, and it's what lets nvim's TUI
 * start. Redirections inside such a session still look like ttys (ceiling). */
extern char *getenv(const char *);
int isatty(int fd){ return fd >= 0 && fd <= 2 && getenv("TERM") != 0; }

/* ioctl: the winsize pair is real (backed by the /env/LINES + /env/COLS files
 * alacritty9 publishes and updates live); everything else is ENOTTY. */
static int env_num(const char *name){
	char path[64], buf[16];
	char *d = path; const char *p = "/env/";
	while(*p) *d++ = *p++;
	while(*name) *d++ = *name++;
	*d = 0;
	long fd = n9_open(path, 0);
	if(fd < 0) return 0;
	long n = n9_pread((int)fd, buf, sizeof buf - 1, 0);
	n9_close((int)fd);
	if(n <= 0) return 0;
	buf[n] = 0;
	int v = 0; for(char *s = buf; *s >= '0' && *s <= '9'; s++) v = v*10 + (*s - '0');
	return v;
}
/* GPU driver hook. iris (and any DRM client) issues ioctls with type byte 'd'
 * (0x64). If a gpu9 handler is linked in, route those to it — that IS the
 * "kernel driver", in-process. Weak, so ordinary cc9 programs are unaffected. */
extern int gpu9_ioctl(int fd, unsigned long req, void *arg) __attribute__((weak));

int ioctl(int fd, unsigned long req, ...){
	__builtin_va_list ap; __builtin_va_start(ap, req);
	void *arg = __builtin_va_arg(ap, void *);
	__builtin_va_end(ap);
	if(gpu9_ioctl != 0 && ((req >> 8) & 0xff) == 0x64 /* DRM_IOCTL_BASE 'd' */)
		return gpu9_ioctl(fd, req, arg);
	(void)fd;
	if(req == 0x5421 /*FIONBIO*/){
		extern int fcntl(int, int, ...);
		int on = arg ? *(int *)arg : 0;
		return fcntl(fd, 4 /*F_SETFL*/, on ? 0x1000 /*O_NONBLOCK*/ : 0);
	}
	if(req == 0x5413 /*TIOCGWINSZ*/){
		struct { unsigned short r, c, xp, yp; } *ws = arg;
		int rows = env_num("LINES"), cols = env_num("COLS");
		ws->r = rows ? (unsigned short)rows : 24;
		ws->c = cols ? (unsigned short)cols : 80;
		ws->xp = ws->yp = 0;
		return 0;
	}
	if(req == 0x5414 /*TIOCSWINSZ*/) return 0;
	errno = ENOTTY;
	return -1;
}
int fsync(int fd){ (void)fd; return 0; }
int ftruncate(int fd, long len){ unsigned char b[128]; int n=build_wstat(b,0,0xFFFFFFFF,(unsigned long long)len,0xFFFFFFFF); return n9_fwstat(fd,b,n)<0?-1:0; }
int truncate(const char *p, long len){ unsigned char b[128]; int n=build_wstat(b,0,0xFFFFFFFF,(unsigned long long)len,0xFFFFFFFF); return n9_wstat(p,b,n)<0?-1:0; }
extern long n9_dup(int, int);
int dup(int fd){ int r=(int)n9_dup(fd,-1); if(r<0) errno=EBADF; return r; }

/* rename: Plan 9 wstat changes only the final name component, so this works for
 * same-directory renames; cross-directory -> EXDEV (libc++ falls back to copy). */
static int same_dir(const char *a, const char *b){
	const char *la=a, *lb=b;
	for(const char *p=a;*p;p++) if(*p=='/') la=p;
	for(const char *p=b;*p;p++) if(*p=='/') lb=p;
	long na=la-a, nb=lb-b; if(na!=nb) return 0;
	for(long i=0;i<na;i++) if(a[i]!=b[i]) return 0; return 1;
}
int rename(const char *old, const char *neu){
	if(!same_dir(old,neu)){ errno=EXDEV; return -1; }
	const char *nb=neu; for(const char *p=neu;*p;p++) if(*p=='/') nb=p+1;
	unsigned char b[256]; int n=build_wstat(b,nb,0xFFFFFFFF,~0ULL,0xFFFFFFFF);
	if(n9_wstat(old,b,n)>=0) return 0;
	/* POSIX rename replaces an existing target; Plan 9 wstat-rename refuses if it
	 * exists, so remove it and retry (not atomic, but matches POSIX visibility). */
	struct stat st; if(stat(neu,&st)==0 && n9_remove(neu)>=0 && n9_wstat(old,b,n)>=0) return 0;
	errno=EACCES; return -1;
}
int renameat(int d1,const char*a,int d2,const char*b){ (void)d1;(void)d2; return rename(a,b); }
int link(const char *a, const char *b){ (void)a;(void)b; errno=ENOSYS; return -1; }
int linkat(int d1,const char*a,int d2,const char*b,int f){ (void)d1;(void)a;(void)d2;(void)b;(void)f; errno=ENOSYS; return -1; }
int symlink(const char *a, const char *b){ (void)a;(void)b; errno=ENOSYS; return -1; }
int mkfifo(const char *a, mode_t m){ (void)a;(void)m; errno=ENOSYS; return -1; }  /* 9front has no POSIX named pipes */
int symlinkat(const char*a,int d,const char*b){ (void)a;(void)d;(void)b; errno=ENOSYS; return -1; }
long readlink(const char *p, char *b, size_t n){ (void)p;(void)b;(void)n; errno=EINVAL; return -1; }
long readlinkat(int d,const char*p,char*b,size_t n){ (void)d;(void)p;(void)b;(void)n; errno=EINVAL; return -1; }
char *realpath(const char *p, char *out){ if(out){ size_t i=0; while(p[i]){out[i]=p[i];i++;} out[i]=0; return out; } return 0; }
char *getcwd(char *buf, size_t n){ long fd=n9_open(".",0); if(fd<0)return 0; long r=n9_fd2path((int)fd,buf,(int)n); n9_close((int)fd); return r<0?0:buf; }

/* ---- directories ---- */
struct __cc9_dir { int fd; unsigned char buf[8192]; int len; int pos; struct dirent de; };
/* readdir parses raw bytes from a pread, so it MUST be a real directory; a
 * regular file's contents would be misparsed as Dir entries (and walk OOB). */
static int is_dir_fd(int fd){
	unsigned char sb[512];
	if(n9_fstat(fd, sb, sizeof sb) < 0) return 0;
	return ((unsigned long)le(sb+21,4) & DMDIR) != 0;
}
DIR *opendir(const char *path){
	long fd=n9_open(path, 0); if(fd<0){ errno=ENOENT; return 0; }
	if(!is_dir_fd((int)fd)){ n9_close((int)fd); errno=ENOTDIR; return 0; }
	struct __cc9_dir *d = malloc(sizeof *d); if(!d){ n9_close((int)fd); return 0; }
	d->fd=(int)fd; d->len=0; d->pos=0; return d;
}
DIR *fdopendir(int fd){
	if(!is_dir_fd(fd)){ errno=ENOTDIR; return 0; }
	struct __cc9_dir *d=malloc(sizeof *d); if(!d)return 0; d->fd=fd; d->len=0; d->pos=0; return d;
}
struct dirent *readdir(DIR *d){
	if(d->pos >= d->len){
		long n=n9_pread(d->fd, d->buf, (long)sizeof d->buf, -1);
		if(n<=0) return 0;
		d->len=(int)n; d->pos=0;
	}
	/* bounds-check every field against the read length so a short/garbage read
	 * can't drive an out-of-bounds parse (size[2]@0, mode[4]@21, namelen[2]@41,
	 * name@43). A Dir entry must fit entirely within d->len. */
	if(d->pos + 43 > d->len){ d->pos = d->len; return 0; }
	unsigned char *p = d->buf + d->pos;
	int sz = (int)le(p,2) + 2;                 /* size[2] + body */
	if(sz < 43 || d->pos + sz > d->len){ d->pos = d->len; return 0; }
	int namelen = (int)le(p+41,2);
	if(43 + namelen > sz){ d->pos = d->len; return 0; }   /* name overruns the entry */
	unsigned long mode = (unsigned long)le(p+21,4);
	int i; for(i=0;i<namelen && i<255;i++) d->de.d_name[i]=(char)p[43+i];
	d->de.d_name[i]=0;
	d->de.d_ino = le(p+13,8);
	d->de.d_type = (mode & DMDIR) ? DT_DIR : DT_REG;
	d->pos += sz;
	return &d->de;
}
int closedir(DIR *d){ int fd=d->fd; free(d); return n9_close(fd)<0?-1:0; }
int dirfd(DIR *d){ return d->fd; }
void rewinddir(DIR *d){ long long r; n9_seek(&r,d->fd,0,0); d->len=0; d->pos=0; }

/* file-time setting via wstat. Plan 9 can only set mtime (atime is kernel-owned),
 * so t[0] (atime) is accepted and ignored; t==0 (POSIX "set to now") is a no-op —
 * every real caller passes explicit times. */
#include <time.h>
#include <sys/time.h>
struct statvfs;
#define CC9_UTIME_OMIT ((long)((1L<<30)-2))   /* glibc UTIME_OMIT */
int utimes(const char *p, const struct timeval *t){
	if(!t) return 0;
	return cc9_set_mtime(p, (unsigned long)t[1].tv_sec);
}
/* legacy POSIX spelling (see include/utime.h); Plan 9 wstat only sets mtime */
struct utimbuf { long actime; long modtime; };
int utime(const char *p, const struct utimbuf *t){
	if(!t) return 0;
	return cc9_set_mtime(p, (unsigned long)t->modtime);
}
int utimensat(int d, const char *p, const struct timespec *t, int f){
	(void)f; char b[1024];
	if(!t || t[1].tv_nsec == CC9_UTIME_OMIT) return 0;
	return cc9_set_mtime(at_path(d,p,b,sizeof b), (unsigned long)t[1].tv_sec);
}
int futimens(int fd, const struct timespec *t){
	if(!t || t[1].tv_nsec == CC9_UTIME_OMIT) return 0;
	return cc9_fset_mtime(fd, (unsigned long)t[1].tv_sec);
}
/* statvfs: report a generic large filesystem (the values most code only sanity-checks). */
struct __cc9_statvfs { unsigned long f_bsize,f_frsize,f_blocks,f_bfree,f_bavail,f_files,f_ffree,f_favail,f_fsid,f_flag,f_namemax; };
int statvfs(const char *p, void *vp){ (void)p; struct __cc9_statvfs *s=vp; memset(s,0,sizeof *s); s->f_bsize=8192; s->f_frsize=8192; s->f_blocks=1<<20; s->f_bfree=1<<19; s->f_bavail=1<<19; s->f_namemax=255; return 0; }
int fstatvfs(int fd, void *vp){ (void)fd; return statvfs(0,vp); }

/* getenv/setenv via Plan 9 /env/<name> (the per-process environment namespace). */
static void env_path(const char *name, char *path, int n){
	const char *pfx="/env/"; int i=0; while(pfx[i]){ path[i]=pfx[i]; i++; }
	int j=0; while(name[j] && i<n-1){ path[i++]=name[j++]; } path[i]=0;
}
/* POSIX-compat defaults for vars Plan 9 spells differently ($path is a NUL-
 * separated rc list; there is no $SHELL). POSIX code (nvim jobstart/'shell',
 * execvp users) expects these; /bin is the 9front union bin so a single-entry
 * PATH covers everything. A real /env/PATH or /env/SHELL overrides. */
static const char *env_default(const char *name){
	if(name[0]=='P'&&name[1]=='A'&&name[2]=='T'&&name[3]=='H'&&!name[4]) return "/bin";
	if(name[0]=='S'&&name[1]=='H'&&name[2]=='E'&&name[3]=='L'&&name[4]=='L'&&!name[5]) return "/bin/rc";
	return 0;
}
char *getenv(const char *name){
	static char val[1024]; char path[300];
	env_path(name, path, sizeof path);
	long fd=n9_open(path, 0); if(fd<0) return (char*)env_default(name);
	long n=n9_pread((int)fd, val, sizeof val - 1, -1); n9_close((int)fd);
	if(n<=0) return (char*)env_default(name);
	while(n>0 && (val[n-1]=='\n'||val[n-1]==0)) n--;   /* /env values may be NUL/NL-terminated */
	val[n]=0; return val;
}
/* setenv/unsetenv write the /env file so getenv() round-trips (the libc++
 * temp_directory_path test sets TMPDIR then expects it back). create() truncates
 * an existing /env file, giving overwrite semantics. */
int setenv(const char *n, const char *v, int overwrite){
	if(!n || !*n){ errno=EINVAL; return -1; }
	char path[300]; env_path(n, path, sizeof path);
	if(!overwrite){ long e=n9_open(path,0); if(e>=0){ n9_close((int)e); return 0; } }
	long fd=n9_create(path, 1/*OWRITE*/, 0666); if(fd<0){ errno=EACCES; return -1; }
	long len = v ? (long)strlen(v) : 0;
	if(len>0 && n9_pwrite((int)fd, v, len, 0) < len){ n9_close((int)fd); errno=EIO; return -1; }
	n9_close((int)fd); return 0;
}
int unsetenv(const char *n){
	if(!n || !*n){ errno=EINVAL; return -1; }
	char path[300]; env_path(n, path, sizeof path);
	n9_remove(path);   /* removing an absent var is not an error */
	return 0;
}
/* putenv("K=V"). NB: POSIX says the caller's string BECOMES the environment
 * (so later edits to it show through, and it must not be freed). That is
 * unimplementable here — the environment is /env files, not an in-process
 * environ[] that could alias the string — so we split and setenv (i.e. copy).
 * Every program that isn't deliberately mutating the string afterward can't
 * tell the difference. A missing '=' is EINVAL (POSIX allows treating
 * "NAME" as an unset; the split form is the common reading). */
int putenv(char *s){
	if(!s){ errno=EINVAL; return -1; }
	const char *eq=s; while(*eq && *eq!='=') eq++;
	if(*eq!='='){ errno=EINVAL; return -1; }
	char name[256]; long n=eq-s;
	if(n<=0 || n>=(long)sizeof name){ errno=EINVAL; return -1; }
	for(long i=0;i<n;i++) name[i]=s[i];
	name[n]=0;
	return setenv(name, eq+1, 1);
}

/* POSIX `environ`. Plan 9 keeps the environment as files under /env (no env
 * array on the entry stack), so crt0 calls this once at startup to materialize
 * a NULL-terminated "name=value" array from /env. getenv() still reads /env
 * directly (authoritative); this is only for code that walks `environ`. */
char **environ = 0;
void __cc9_build_environ(void){
	/* ponytail: 512-entry cap; /env rarely holds more. Grow if it ever does. */
	static char *tab[512];
	DIR *d = opendir("/env"); if(!d){ tab[0]=0; environ=tab; return; }
	int n=0; struct dirent *e;
	while(n < 511 && (e=readdir(d))){
		char name[256]; int i=0; for(; e->d_name[i] && i<255; i++) name[i]=e->d_name[i]; name[i]=0;
		if(name[0]=='.' && (name[1]==0 || (name[1]=='.'&&name[2]==0))) continue;
		char *v = getenv(name);          /* shared static — consume before next readdir/getenv */
		int vl = 0; while(v && v[vl]) vl++;
		char *kv = (char*)malloc(i + 1 + vl + 1); if(!kv) break;
		int k=0; for(int j=0;j<i;j++) kv[k++]=name[j]; kv[k++]='=';
		for(int j=0;j<vl;j++) kv[k++]=v[j]; kv[k]=0;
		tab[n++]=kv;
	}
	/* surface the POSIX-compat defaults in the array too */
	{
		static const char *dflt[] = { "PATH", "SHELL", 0 };
		for(int di = 0; dflt[di] && n < 511; di++){
			int seen = 0;
			int dl = 0; while(dflt[di][dl]) dl++;
			for(int t = 0; t < n; t++){
				const char *kv = tab[t]; int j = 0;
				while(j < dl && kv[j] == dflt[di][j]) j++;
				if(j == dl && kv[j] == '='){ seen = 1; break; }
			}
			if(seen) continue;
			const char *v = env_default(dflt[di]);
			int vl = 0; while(v[vl]) vl++;
			char *kv = (char*)malloc(dl + 1 + vl + 1); if(!kv) break;
			int k = 0;
			for(int j = 0; j < dl; j++) kv[k++] = dflt[di][j];
			kv[k++] = '=';
			for(int j = 0; j < vl; j++) kv[k++] = v[j];
			kv[k] = 0;
			tab[n++] = kv;
		}
	}
	tab[n]=0; closedir(d); environ=tab;
}

/* mkstemp: replace the trailing "XXXXXX" with a unique suffix and create the
 * file O_EXCL (reusing open()). Seed uniqueness from /dev/random (kernel
 * entropy); a static counter keeps successive calls in one process
 * distinct. The libc++ test framework (platform_support.h) needs this. */
int mkstemp(char *tmpl){
	int len = tmpl ? (int)strlen(tmpl) : 0;
	if(len < 6){ errno = EINVAL; return -1; }
	char *x = tmpl + len - 6;
	for(int i=0;i<6;i++) if(x[i] != 'X'){ errno = EINVAL; return -1; }
	static const char cs[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
	static unsigned long seq = 0;
	unsigned long seed = 0;
	int rf = n9_open("/dev/random", 0);
	if(rf >= 0){ n9_pread(rf, &seed, sizeof seed, -1); n9_close(rf); }
	for(int attempt=0; attempt<4096; attempt++){
		unsigned long v = seed + (seq++ * 0x9E3779B97F4A7C15UL) + (unsigned)attempt;
		for(int i=0;i<6;i++){ x[i] = cs[v % 62]; v /= 62; }
		int fd = open(tmpl, O_RDWR|O_CREAT|O_EXCL, 0600);
		if(fd >= 0) return fd;
	}
	errno = EEXIST;
	return -1;
}

/* writev/readv (sys/uio.h). Plan 9 has no scatter/gather syscall, so both stage
 * through one buffer and do exactly ONE read/write.
 *
 * That single call is the point, not an optimisation. These started out serving
 * libuv's stream writes, where "writev loops each iov" and "readv fills only
 * iov[0], callers loop" are both defensible — a short read on a stream just means
 * read again. net9.c's sendmsg/recvmsg now route socket I/O through them, and on
 * a SOCK_DGRAM neither holds: one write per iovec turns a single datagram into N
 * packets, and reading into only the first iovec truncates the datagram and drops
 * the remainder, because the unread rest of a packet is discarded rather than
 * queued. One syscall per call keeps a datagram a datagram.
 *
 * Returning short is still legal for a stream, and callers must still loop. */
struct iovec { void *iov_base; size_t iov_len; };
extern void *memcpy(void *, const void *, size_t);

/* Total across the iovec array; reports how many are non-empty and the first such
 * (the single-buffer fast path skips staging entirely). -1 on length overflow. */
static long iov_span(const struct iovec *iov, int n, int *nonempty, int *first){
	unsigned long t = 0;
	*nonempty = 0; *first = -1;
	for(int i = 0; i < n; i++){
		unsigned long l = iov[i].iov_len;
		if(!l) continue;
		if(*first < 0) *first = i;
		(*nonempty)++;
		if(t + l < t) return -1;
		t += l;
	}
	return (long)t;
}

long writev(int fd, const struct iovec *iov, int n){
	if(n < 0 || (n > 0 && !iov)){ errno = EINVAL; return -1; }
	int ne, first;
	long total = iov_span(iov, n, &ne, &first);
	if(total < 0){ errno = EINVAL; return -1; }
	if(ne == 0) return 0;
	if(ne == 1) return write(fd, iov[first].iov_base, iov[first].iov_len);

	char *tmp = malloc((size_t)total);
	if(!tmp){ errno = ENOMEM; return -1; }
	long off = 0;
	for(int i = 0; i < n; i++){
		if(!iov[i].iov_len) continue;
		memcpy(tmp + off, iov[i].iov_base, iov[i].iov_len);
		off += (long)iov[i].iov_len;
	}
	long w = write(fd, tmp, (size_t)total);
	free(tmp);
	return w;
}

long readv(int fd, const struct iovec *iov, int n){
	if(n < 0 || (n > 0 && !iov)){ errno = EINVAL; return -1; }
	int ne, first;
	long total = iov_span(iov, n, &ne, &first);
	if(total < 0){ errno = EINVAL; return -1; }
	if(ne == 0) return 0;
	if(ne == 1) return read(fd, iov[first].iov_base, iov[first].iov_len);

	char *tmp = malloc((size_t)total);
	if(!tmp){ errno = ENOMEM; return -1; }
	long r = read(fd, tmp, (size_t)total);
	if(r > 0){
		long off = 0, left = r;
		for(int i = 0; i < n && left > 0; i++){
			unsigned long take = iov[i].iov_len < (unsigned long)left
			                   ? iov[i].iov_len : (unsigned long)left;
			if(!take) continue;
			memcpy(iov[i].iov_base, tmp + off, take);
			off += (long)take;
			left -= (long)take;
		}
	}
	free(tmp);
	return r;
}

/* mkdtemp — trailing XXXXXX replaced from /dev/random, then mkdir. */
extern int mkdir(const char *, unsigned int);
extern int rand(void);
char *mkdtemp(char *tpl){
	size_t len = strlen(tpl);
	if(len < 6){ errno = EINVAL; return 0; }
	char *x = tpl + len - 6;
	for(int attempt = 0; attempt < 100; attempt++){
		for(int i = 0; i < 6; i++) x[i] = 'a' + rand() % 26;
		if(mkdir(tpl, 0700) == 0) return tpl;
	}
	errno = EEXIST;
	return 0;
}

/* scandir/alphasort — over opendir/readdir, for libuv's uv_fs_scandir. */
extern void *realloc(void *, size_t);
extern int strcmp(const char *, const char *);
extern void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
int alphasort(const struct dirent **a, const struct dirent **b){
	return strcmp((*a)->d_name, (*b)->d_name);
}
int scandir(const char *path, struct dirent ***out,
            int (*filter)(const struct dirent *),
            int (*cmp)(const struct dirent **, const struct dirent **)){
	DIR *d = opendir(path);
	if(!d) return -1;
	struct dirent **v = 0; int n = 0, cap = 0;
	struct dirent *e;
	while((e = readdir(d))){
		if(filter && !filter(e)) continue;
		if(n == cap){
			cap = cap ? cap*2 : 16;
			struct dirent **nv = realloc(v, cap * sizeof *nv);
			if(!nv) goto fail;
			v = nv;
		}
		size_t sz = sizeof(struct dirent);
		struct dirent *c = malloc(sz);
		if(!c) goto fail;
		memcpy(c, e, sz);
		v[n++] = c;
	}
	closedir(d);
	if(cmp && n > 1) qsort(v, n, sizeof *v, (int (*)(const void *, const void *))cmp);
	*out = v;
	return n;
fail:
	while(n) free(v[--n]);
	free(v); closedir(d);
	errno = ENOMEM;
	return -1;
}
