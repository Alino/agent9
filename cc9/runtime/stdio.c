/* cc9 stdio — a minimal FILE layer over Plan 9 fds, enough to back libc++'s
 * std::cout/cin/cerr (std_stream.h wraps a FILE* with fwrite/fread/fgetc/
 * ungetc/fflush) and the C stdio surface. Line-unbuffered: each write is a
 * pwrite syscall (correct, if not the fastest). */
typedef unsigned long size_t;
extern long n9_pwrite(int, const void *, long, long long);
extern long n9_pread(int, void *, long, long long);
extern void *malloc(size_t);
extern void free(void *);
extern void *memcpy(void *, const void *, size_t);

/* mem != 0 → memory-backed (fmemopen); fd is -1 then. mpos/msize track the
 * caller's buffer. fd-backed FILEs leave mem zero. */
struct _CC9_FILE { int fd; int ungot; int eof; int err; char *mem; size_t mpos; size_t msize; };
typedef struct _CC9_FILE FILE;
static FILE _stdin = {0, -1, 0, 0}, _stdout = {1, -1, 0, 0}, _stderr = {2, -1, 0, 0};
FILE *stdin = &_stdin, *stdout = &_stdout, *stderr = &_stderr;

size_t fwrite(const void *p, size_t sz, size_t n, FILE *f){
	if(!sz || !n) return 0;
	size_t total = sz * n, done = 0;
	const char *s = p;
	if(f->mem){   /* memory-backed: append into the caller's buffer, up to msize */
		size_t avail = f->msize - f->mpos, want = total < avail ? total : avail;
		memcpy(f->mem + f->mpos, s, want); f->mpos += want;
		return want / sz;
	}
	/* loop: one pwrite can short-write on pipes/devices/9P mounts; keep going
	 * until everything is written or an error, else the tail is silently lost. */
	while(done < total){
		long w = n9_pwrite(f->fd, s + done, (long)(total - done), -1);
		if(w <= 0){ f->err = 1; break; }
		done += (size_t)w;
	}
	return done / sz;
}
size_t fread(void *p, size_t sz, size_t n, FILE *f){
	if(!sz || !n) return 0;
	char *d = p; size_t total = sz * n, got = 0;
	if(f->mem){   /* memory-backed: read from the caller's buffer */
		size_t avail = f->msize - f->mpos, want = total < avail ? total : avail;
		if(want < total) f->eof = 1;
		memcpy(d, f->mem + f->mpos, want); f->mpos += want;
		return want / sz;
	}
	if(f->ungot >= 0){ d[got++] = (char)f->ungot; f->ungot = -1; }
	/* loop until the request is satisfied or EOF/error (a single pread can return
	 * short on pipes/devices even when more data is coming). */
	while(got < total){
		long r = n9_pread(f->fd, d + got, (long)(total - got), -1);
		if(r < 0){ f->err = 1; break; }
		if(r == 0){ f->eof = 1; break; }
		got += (size_t)r;
	}
	return got / sz;
}
int fputc(int c, FILE *f){ unsigned char b = (unsigned char)c; return fwrite(&b,1,1,f)==1 ? (int)b : -1; }
int putc(int c, FILE *f){ return fputc(c,f); }
int putchar(int c){ return fputc(c, stdout); }
int fgetc(FILE *f){ unsigned char b; if(f->ungot>=0){ int u=f->ungot; f->ungot=-1; return u; } if(f->mem){ if(f->mpos>=f->msize){ f->eof=1; return -1; } return (unsigned char)f->mem[f->mpos++]; } long r=n9_pread(f->fd,&b,1,-1); if(r<=0){ f->eof=(r==0); return -1; } return b; }
int getc(FILE *f){ return fgetc(f); }
int getchar(void){ return fgetc(stdin); }
int ungetc(int c, FILE *f){ if(c==-1) return -1; f->ungot = (unsigned char)c; f->eof = 0; return c; }
int fputs(const char *s, FILE *f){ size_t n=0; while(s[n])n++; return fwrite(s,1,n,f)==n ? 0 : -1; }
int puts(const char *s){ if(fputs(s,stdout)<0) return -1; return fputc('\n',stdout); }
char *fgets(char *s, int n, FILE *f){ int i=0; while(i<n-1){ int c=fgetc(f); if(c==-1){ if(i==0) return 0; break; } s[i++]=(char)c; if(c=='\n') break; } s[i]=0; return s; }
int fflush(FILE *f){ (void)f; return 0; }   /* unbuffered: nothing pending */
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
extern long n9_seek(long long *, int, long long, int);
extern long n9_stat(const char *, unsigned char *, int);
extern void *malloc(size_t); extern void free(void *);
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
	if(fd<0) return 0;
	FILE *f=malloc(sizeof *f); if(!f){ n9_close((int)fd); return 0; }
	f->fd=(int)fd; f->ungot=-1; f->eof=0; f->err=0; f->mem=0; f->mpos=0; f->msize=0;
	if(append){ long long r; n9_seek(&r,(int)fd,0,2); }
	return f;
}
FILE *fdopen(int fd, const char *mode){ (void)mode; FILE *f=malloc(sizeof *f); if(!f)return 0; f->fd=fd; f->ungot=-1; f->eof=0; f->err=0; f->mem=0; f->mpos=0; f->msize=0; return f; }
FILE *freopen(const char *path, const char *mode, FILE *f){ if(!f)return 0; if(f->fd>2)n9_close(f->fd); FILE *nf=fopen(path,mode); if(!nf)return 0; f->fd=nf->fd; f->ungot=-1; f->eof=0; f->err=0; f->mem=0; f->mpos=0; f->msize=0; free(nf); return f; }
/* fmemopen: a FILE* backed by the caller's buffer. ponytail: buf must be
 * non-null and modes start at pos 0 (no 'a'/null-buf alloc) — all the tests need. */
FILE *fmemopen(void *buf, size_t size, const char *mode){
	(void)mode; if(!buf) return 0;
	FILE *f=malloc(sizeof *f); if(!f) return 0;
	f->fd=-1; f->ungot=-1; f->eof=0; f->err=0; f->mem=buf; f->mpos=0; f->msize=size;
	return f;
}
int fclose(FILE *f){ if(!f) return 0; if(f==stdin||f==stdout||f==stderr) return 0; int fd=f->fd; int mem=(f->mem!=0); free(f); if(mem) return 0; return n9_close(fd)<0?-1:0; }
long ftell(FILE *f){ if(f->mem) return (long)f->mpos; long long r=0; if(n9_seek(&r,f->fd,0,1)<0) return -1; return (long)r; }
long ftello(FILE *f){ return ftell(f); }
int fseek(FILE *f, long off, int whence){ long long r; f->ungot=-1; f->eof=0;
	if(f->mem){ size_t base = whence==1 ? f->mpos : whence==2 ? f->msize : 0; f->mpos = base + (size_t)off; return 0; }
	return n9_seek(&r,f->fd,off,whence)<0?-1:0; }
int fseeko(FILE *f, long off, int whence){ return fseek(f,off,whence); }
void rewind(FILE *f){ fseek(f,0,0); }
int fgetpos(FILE *f, void *pos){ long *p=pos; *p=ftell(f); return 0; }
int fsetpos(FILE *f, const void *pos){ const long *p=pos; return fseek(f,*p,0); }
int setvbuf(FILE *f, char *b, int m, size_t s){ (void)f;(void)b;(void)m;(void)s; return 0; }
void setbuf(FILE *f, char *b){ (void)f;(void)b; }
