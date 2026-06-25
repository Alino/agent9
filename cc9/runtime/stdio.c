/* cc9 stdio — a minimal FILE layer over Plan 9 fds, enough to back libc++'s
 * std::cout/cin/cerr (std_stream.h wraps a FILE* with fwrite/fread/fgetc/
 * ungetc/fflush) and the C stdio surface. Line-unbuffered: each write is a
 * pwrite syscall (correct, if not the fastest). */
typedef unsigned long size_t;
extern long n9_pwrite(int, const void *, long, long long);
extern long n9_pread(int, void *, long, long long);

struct _CC9_FILE { int fd; int ungot; int eof; int err; };
typedef struct _CC9_FILE FILE;
static FILE _stdin = {0, -1, 0, 0}, _stdout = {1, -1, 0, 0}, _stderr = {2, -1, 0, 0};
FILE *stdin = &_stdin, *stdout = &_stdout, *stderr = &_stderr;

size_t fwrite(const void *p, size_t sz, size_t n, FILE *f){
	if(!sz || !n) return 0;
	long total = (long)(sz * n);
	long w = n9_pwrite(f->fd, p, total, -1);
	if(w < 0){ f->err = 1; return 0; }
	return (size_t)w / sz;
}
size_t fread(void *p, size_t sz, size_t n, FILE *f){
	if(!sz || !n) return 0;
	char *d = p; size_t got = 0;
	if(f->ungot >= 0){ *d++ = (char)f->ungot; f->ungot = -1; got = 1; }
	long want = (long)(sz*n) - (long)got;
	if(want > 0){
		long r = n9_pread(f->fd, d, want, -1);
		if(r < 0){ f->err = 1; }
		else { if(r == 0) f->eof = 1; got += (size_t)r; }
	}
	return got / sz;
}
int fputc(int c, FILE *f){ unsigned char b = (unsigned char)c; return fwrite(&b,1,1,f)==1 ? (int)b : -1; }
int putc(int c, FILE *f){ return fputc(c,f); }
int putchar(int c){ return fputc(c, stdout); }
int fgetc(FILE *f){ unsigned char b; if(f->ungot>=0){ int u=f->ungot; f->ungot=-1; return u; } long r=n9_pread(f->fd,&b,1,-1); if(r<=0){ f->eof=(r==0); return -1; } return b; }
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
int fileno(FILE *f){ return f->fd; }

/* printf family over vsnprintf (n9libc) + fwrite. */
extern int vsnprintf(char *, size_t, const char *, __builtin_va_list);
int vfprintf(FILE *f, const char *fmt, __builtin_va_list ap){
	char buf[1024]; int n = vsnprintf(buf, sizeof buf, fmt, ap);
	if(n > (int)sizeof buf - 1) n = sizeof buf - 1;
	return (int)fwrite(buf, 1, (size_t)n, f);
}
int fprintf(FILE *f, const char *fmt, ...){ __builtin_va_list ap; __builtin_va_start(ap,fmt); int r=vfprintf(f,fmt,ap); __builtin_va_end(ap); return r; }
int printf(const char *fmt, ...){ __builtin_va_list ap; __builtin_va_start(ap,fmt); int r=vfprintf(stdout,fmt,ap); __builtin_va_end(ap); return r; }
int vprintf(const char *fmt, __builtin_va_list ap){ return vfprintf(stdout, fmt, ap); }
/* sscanf: minimal — enough for the few locale code paths that touch it. */
int vsscanf(const char *s, const char *f, __builtin_va_list ap){ (void)s;(void)f;(void)ap; return 0; }
int sscanf(const char *s, const char *f, ...){ (void)s;(void)f; return 0; }

/* real files over Plan 9 (backs std::fstream / basic_filebuf). */
extern long n9_open(const char *, int);
extern long n9_create(const char *, int, unsigned long);
extern long n9_close(int);
extern long n9_seek(long long *, int, long long, int);
extern void *malloc(size_t); extern void free(void *);
FILE *fopen(const char *path, const char *mode){
	int omode=0, creat=0, trunc=0, append=0, plus=0;
	for(const char *m=mode; *m; m++) if(*m=='+') plus=1;
	if(mode[0]=='r') omode = plus?2:0;
	else if(mode[0]=='w'){ omode = plus?2:1; creat=1; trunc=1; }
	else if(mode[0]=='a'){ omode = plus?2:1; creat=1; append=1; }
	long fd = creat ? n9_create(path, omode|(trunc?16:0), 0666) : n9_open(path, omode);
	if(fd<0) return 0;
	FILE *f=malloc(sizeof *f); if(!f){ n9_close((int)fd); return 0; }
	f->fd=(int)fd; f->ungot=-1; f->eof=0; f->err=0;
	if(append){ long long r; n9_seek(&r,(int)fd,0,2); }
	return f;
}
FILE *fdopen(int fd, const char *mode){ (void)mode; FILE *f=malloc(sizeof *f); if(!f)return 0; f->fd=fd; f->ungot=-1; f->eof=0; f->err=0; return f; }
FILE *freopen(const char *path, const char *mode, FILE *f){ if(!f)return 0; if(f->fd>2)n9_close(f->fd); FILE *nf=fopen(path,mode); if(!nf)return 0; f->fd=nf->fd; f->ungot=-1; f->eof=0; f->err=0; free(nf); return f; }
int fclose(FILE *f){ if(!f) return 0; if(f==stdin||f==stdout||f==stderr) return 0; int fd=f->fd; free(f); return n9_close(fd)<0?-1:0; }
long ftell(FILE *f){ long long r=0; if(n9_seek(&r,f->fd,0,1)<0) return -1; return (long)r; }
long ftello(FILE *f){ return ftell(f); }
int fseek(FILE *f, long off, int whence){ long long r; f->ungot=-1; f->eof=0; return n9_seek(&r,f->fd,off,whence)<0?-1:0; }
int fseeko(FILE *f, long off, int whence){ return fseek(f,off,whence); }
void rewind(FILE *f){ fseek(f,0,0); }
int fgetpos(FILE *f, void *pos){ long *p=pos; *p=ftell(f); return 0; }
int fsetpos(FILE *f, const void *pos){ const long *p=pos; return fseek(f,*p,0); }
int setvbuf(FILE *f, char *b, int m, size_t s){ (void)f;(void)b;(void)m;(void)s; return 0; }
void setbuf(FILE *f, char *b){ (void)f;(void)b; }
