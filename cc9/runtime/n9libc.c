typedef unsigned long size_t;
extern void n9_exits(const char*);
static char *utoa_(unsigned long long v, char *p, int base);  /* defined below */

/* Heap over the Plan 9 brk syscall: grows real memory from the kernel (no
 * fixed cap). `end` is the end of bss (linker symbol); the break starts there.
 * Bump allocator that sbrk's more in >=1MB chunks (contiguous, so hend just
 * grows). free() is a no-op for now (a free-list is the next refinement). */
extern char end[];
extern long n9_brk(void *);
static char *cur_brk = 0;

static void *n9_sbrk(long incr){
	if(!cur_brk) cur_brk = (char*)(((unsigned long)end + 0xfff) & ~0xfffUL);
	char *old = cur_brk;
	if(incr && n9_brk(cur_brk + incr) < 0) return (void*)-1;
	cur_brk += incr;
	return old;
}

/* K&R free-list allocator: free() reclaims and coalesces (no more leak). */
typedef union header { struct { union header *ptr; size_t size; } s; long a; } Header;
static Header kr_base;
static Header *kr_freep = 0;

/* aligned_alloc (C11) stamps this sentinel at ap[-1] and the real malloc base
 * at ap[-2], so plain free() can recover and release the underlying block. The
 * value is huge; a normal block's ap[-1] is a small Header.size unit count, so
 * there is no collision. */
#define CC9_ALIGN_MAGIC 0xA11C9EDA11C9EDUL

void free(void *ap){
	if(!ap) return;
	if(((unsigned long*)ap)[-1] == CC9_ALIGN_MAGIC) ap = *(void**)((char*)ap - 16);
	Header *bp = (Header*)ap - 1, *p;
	for(p = kr_freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr)
		if(p >= p->s.ptr && (bp > p || bp < p->s.ptr)) break;
	if(bp + bp->s.size == p->s.ptr){ bp->s.size += p->s.ptr->s.size; bp->s.ptr = p->s.ptr->s.ptr; }
	else bp->s.ptr = p->s.ptr;
	if(p + p->s.size == bp){ p->s.size += bp->s.size; p->s.ptr = bp->s.ptr; }
	else p->s.ptr = bp;
	kr_freep = p;
}
static Header *kr_morecore(size_t nu){
	if(nu < 4096) nu = 4096;
	void *cp = n9_sbrk((long)(nu * sizeof(Header)));
	if(cp == (void*)-1) return 0;
	Header *up = (Header*)cp; up->s.size = nu;
	free((void*)(up + 1));
	return kr_freep;
}
void *malloc(size_t nbytes){
	if(nbytes == 0) return 0;
	size_t nunits = (nbytes + sizeof(Header) - 1) / sizeof(Header) + 1;
	Header *p, *prevp;
	if((prevp = kr_freep) == 0){ kr_base.s.ptr = kr_freep = prevp = &kr_base; kr_base.s.size = 0; }
	for(p = prevp->s.ptr; ; prevp = p, p = p->s.ptr){
		if(p->s.size >= nunits){
			if(p->s.size == nunits) prevp->s.ptr = p->s.ptr;
			else { p->s.size -= nunits; p += p->s.size; p->s.size = nunits; }
			kr_freep = prevp;
			return (void*)(p + 1);
		}
		if(p == kr_freep && (p = kr_morecore(nunits)) == 0) return 0;
	}
}
/* aligned_alloc(alignment, size) (C11). malloc is 16-byte aligned, so small
 * alignments are free; larger ones over-allocate and align, recording the base
 * + sentinel so plain free() reclaims it (see free above). */
void *aligned_alloc(size_t al, size_t size){
	if(al <= 16) return malloc(size);
	char *base = malloc(size + al + 16);
	if(!base) return 0;
	unsigned long a = ((unsigned long)base + 16 + al - 1) & ~(al - 1);
	*(void**)(a - 16) = base;
	((unsigned long*)a)[-1] = CC9_ALIGN_MAGIC;
	return (void*)a;
}
int posix_memalign(void **out, size_t al, size_t size){
	void *p = aligned_alloc(al < 16 ? 16 : al, size);
	if(!p) return 12 /*ENOMEM*/; *out = p; return 0;
}
void *calloc(size_t a,size_t b){ size_t n=a*b; char*p=malloc(n); if(p) for(size_t i=0;i<n;i++)p[i]=0; return p; }
void *realloc(void*old,size_t n){
	if(!old) return malloc(n);
	if(n==0){ free(old); return 0; }
	Header *bp=(Header*)old-1; size_t oldsz=(bp->s.size-1)*sizeof(Header);
	char *np=malloc(n); if(!np) return 0;
	size_t c=oldsz<n?oldsz:n, i; char*o=old; for(i=0;i<c;i++) np[i]=o[i];
	free(old); return np;
}
void abort(void){ n9_exits("cc9: abort\n"); for(;;){} }
void exit(int code){ n9_exits(code? "cc9: exit nonzero\n" : 0); for(;;){} }
void _Exit(int code){ exit(code); }
/* __atomic_is_lock_free(size, ptr): native scalar widths (<=16B) are lock-free
 * on amd64. Builtin name, so define via an __asm__ label (see __atomic_* below). */
int cc9_atomic_is_lock_free(size_t n, const volatile void *p) __asm__("__atomic_is_lock_free");
int cc9_atomic_is_lock_free(size_t n, const volatile void *p){ (void)p; return n<=16; }
void *memcpy(void*d,const void*s,size_t n){ char*a=d; const char*b=s; for(size_t i=0;i<n;i++)a[i]=b[i]; return d; }
void *memmove(void*d,const void*s,size_t n){ char*a=d; const char*b=s; if(a<b)for(size_t i=0;i<n;i++)a[i]=b[i]; else for(size_t i=n;i>0;i--)a[i-1]=b[i-1]; return d; }
void *memset(void*d,int c,size_t n){ char*a=d; for(size_t i=0;i<n;i++)a[i]=(char)c; return d; }
int memcmp(const void*x,const void*y,size_t n){ const unsigned char*a=x,*b=y; for(size_t i=0;i<n;i++) if(a[i]!=b[i]) return a[i]-b[i]; return 0; }
size_t strlen(const char*s){ size_t n=0; while(s[n])n++; return n; }
char *strcpy(char*d,const char*s){ char*r=d; while((*d++=*s++)); return r; }
char *strncpy(char*d,const char*s,size_t n){ char*r=d; while(n&&(*d=*s)){d++;s++;n--;} while(n--)*d++=0; return r; }
char *strcat(char*d,const char*s){ char*r=d; while(*d)d++; while((*d++=*s++)); return r; }
char *strncat(char*d,const char*s,size_t n){ char*r=d; while(*d)d++; while(n&&*s){*d++=*s++;n--;} *d=0; return r; }
int strcmp(const char*a,const char*b){ while(*a&&*a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }
int strncmp(const char*a,const char*b,size_t n){ while(n&&*a&&*a==*b){a++;b++;n--;} return n?(unsigned char)*a-(unsigned char)*b:0; }
int strcoll(const char*a,const char*b){ return strcmp(a,b); }            /* C locale */
size_t strxfrm(char*d,const char*s,size_t n){ size_t l=strlen(s),i; if(n){ for(i=0;i<n-1&&i<l;i++)d[i]=s[i]; d[i]=0; } return l; }
char *strchr(const char*s,int c){ for(;;s++){ if(*s==(char)c) return (char*)s; if(!*s) return 0; } }
char *strrchr(const char*s,int c){ const char*r=0; for(;;s++){ if(*s==(char)c)r=s; if(!*s) return (char*)r; } }
char *strstr(const char*h,const char*n){ if(!*n) return (char*)h; for(;*h;h++){ const char*a=h,*b=n; while(*a&&*b&&*a==*b){a++;b++;} if(!*b) return (char*)h; } return 0; }
size_t strspn(const char*s,const char*set){ size_t n=0; for(;s[n];n++){ const char*p=set; while(*p&&*p!=s[n])p++; if(!*p) break; } return n; }
size_t strcspn(const char*s,const char*set){ size_t n=0; for(;s[n];n++){ const char*p=set; while(*p&&*p!=s[n])p++; if(*p) break; } return n; }
char *strpbrk(const char*s,const char*set){ for(;*s;s++){ const char*p=set; while(*p){ if(*p==*s) return (char*)s; p++; } } return 0; }
char *strtok(char*s,const char*sep){ static char*save; if(!s)s=save; if(!s)return 0; s+=strspn(s,sep); if(!*s){save=0;return 0;} char*t=s+strcspn(s,sep); if(*t){*t=0;save=t+1;}else save=0; return s; }

/* strerror: common POSIX errno messages (enough for <system_error>'s
 * generic_category().message()). Unknown codes get a generic string. */
char *strerror(int e){
	static char buf[32];
	const char *m = 0;
	switch(e){
	case 0: m="Success"; break;       case 1: m="Operation not permitted"; break;
	case 2: m="No such file or directory"; break; case 3: m="No such process"; break;
	case 4: m="Interrupted system call"; break;   case 5: m="Input/output error"; break;
	case 9: m="Bad file descriptor"; break;        case 11: m="Resource temporarily unavailable"; break;
	case 12: m="Cannot allocate memory"; break;    case 13: m="Permission denied"; break;
	case 14: m="Bad address"; break;               case 16: m="Device or resource busy"; break;
	case 17: m="File exists"; break;               case 19: m="No such device"; break;
	case 20: m="Not a directory"; break;           case 21: m="Is a directory"; break;
	case 22: m="Invalid argument"; break;          case 24: m="Too many open files"; break;
	case 28: m="No space left on device"; break;   case 32: m="Broken pipe"; break;
	case 34: m="Numerical result out of range"; break;
	}
	if(m){ char*d=buf; while(*m)*d++=*m++; *d=0; return buf; }
	char *d=buf; const char*p="Unknown error "; while(*p)*d++=*p++;
	d=utoa_((unsigned long long)(e<0?-e:e), d, 10); *d=0; return buf;
}

/* errno + a C locale (always "C": '.' decimal point) for libc++/json */
static int n9_errno_v = 0;
int *__n9_errno(void){ return &n9_errno_v; }
struct lconv { char *decimal_point, *thousands_sep, *grouping, *p[7]; char c[24]; };
static char dot[] = ".";
static char empty[] = "";
static struct lconv n9_lconv = { dot, empty, empty, {empty,empty,empty,empty,empty,empty,empty} };
struct lconv *localeconv(void){ return &n9_lconv; }
char *setlocale(int c, const char *l){ (void)c; (void)l; return (char*)"C"; }
double strtod(const char *s, char **e){ double r=0; int neg=0; if(*s=='-'){neg=1;s++;} while(*s>='0'&&*s<='9'){r=r*10+(*s-'0');s++;} if(*s=='.'){s++;double f=0.1; while(*s>='0'&&*s<='9'){r+=(*s-'0')*f;f*=0.1;s++;}} if(e)*e=(char*)s; return neg?-r:r; }
long strtol(const char *s, char **e, int b){ (void)b; long r=0; int neg=0; if(*s=='-'){neg=1;s++;} while(*s>='0'&&*s<='9'){r=r*10+(*s-'0');s++;} if(e)*e=(char*)s; return neg?-r:r; }

/* ctype + strtold for libc++/json */
int isdigit(int c){ return c>='0'&&c<='9'; }
int isalpha(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z'); }
int isspace(int c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\v'||c=='\f'; }
int isupper(int c){ return c>='A'&&c<='Z'; }
int islower(int c){ return c>='a'&&c<='z'; }
int isalnum(int c){ return isdigit(c)||isalpha(c); }
int isxdigit(int c){ return isdigit(c)||(c>='a'&&c<='f')||(c>='A'&&c<='F'); }
int tolower(int c){ return isupper(c)?c+32:c; }
int toupper(int c){ return islower(c)?c-32:c; }
int isprint(int c){ return c>=32&&c<127; }
int iscntrl(int c){ return c<32||c==127; }
int isgraph(int c){ return c>32&&c<127; }
int isblank(int c){ return c==' '||c=='\t'; }
int ispunct(int c){ return isgraph(c)&&!isalnum(c); }
long double strtold(const char *s, char **e){ return (long double)strtod(s, e); }

unsigned long long strtoull(const char *s, char **e, int b){ (void)b; unsigned long long r=0; while(*s==' ')s++; while(*s>='0'&&*s<='9'){r=r*10+(*s-'0');s++;} if(e)*e=(char*)s; return r; }
long long strtoll(const char *s, char **e, int b){ (void)b; long long r=0; int neg=0; if(*s=='-'){neg=1;s++;} while(*s>='0'&&*s<='9'){r=r*10+(*s-'0');s++;} if(e)*e=(char*)s; return neg?-r:r; }
unsigned long strtoul(const char *s, char **e, int b){ return (unsigned long)strtoull(s,e,b); }

void *memchr(const void *s, int c, size_t n){ const unsigned char *p=s; for(size_t i=0;i<n;i++) if(p[i]==(unsigned char)c) return (void*)(p+i); return 0; }
float strtof(const char *s, char **e){ return (float)strtod(s, e); }
/* minimal snprintf: enough for libc++/json fallbacks (%d %u %ld %lu %lld %s %c %%). */
static char *utoa_(unsigned long long v, char *p, int base){ char t[32]; int i=0; if(!v)t[i++]='0'; while(v){int d=v%base; t[i++]="0123456789abcdef"[d]; v/=base;} while(i)*p++=t[--i]; return p; }
int vsnprintf(char *out, size_t n, const char *f, __builtin_va_list ap){
  char buf[512]; char *o=buf;
  for(; *f; f++){
    if(*f!='%'){ *o++=*f; continue; }
    f++; int lng=0; while(*f=='l'){lng++;f++;} if(*f=='z')f++;
    if(*f=='d'||*f=='i'){ long long v= lng? __builtin_va_arg(ap,long long): __builtin_va_arg(ap,int); if(v<0){*o++='-';v=-v;} o=utoa_((unsigned long long)v,o,10); }
    else if(*f=='u'){ unsigned long long v= lng? __builtin_va_arg(ap,unsigned long long): __builtin_va_arg(ap,unsigned int); o=utoa_(v,o,10); }
    else if(*f=='x'){ unsigned long long v= lng? __builtin_va_arg(ap,unsigned long long): __builtin_va_arg(ap,unsigned int); o=utoa_(v,o,16); }
    else if(*f=='s'){ const char*s=__builtin_va_arg(ap,const char*); while(*s)*o++=*s++; }
    else if(*f=='c'){ *o++=(char)__builtin_va_arg(ap,int); }
    else if(*f=='%'){ *o++='%'; }
    else { *o++='%'; *o++=*f; }
  }
  *o=0; size_t len=o-buf, i; for(i=0;i+1<n&&i<len;i++) out[i]=buf[i]; if(n)out[i]=0; return (int)len;
}
int snprintf(char *out, size_t n, const char *f, ...){ __builtin_va_list ap; __builtin_va_start(ap,f); int r=vsnprintf(out,n,f,ap); __builtin_va_end(ap); return r; }

/* libm (sqrt/sin/exp/atan2/... + f/l variants) now comes from libcc9m.a
 * (openlibm) — a real correctly-rounded libm with full inf/nan/signbit
 * semantics and 80-bit long double. Only integer abs stays here (openlibm is
 * float-only); the fenv stubs openlibm references live below. */
int abs(int x){ return x<0?-x:x; }
long labs(long x){ return x<0?-x:x; }
long long llabs(long long x){ return x<0?-x:x; }

/* Wall-clock time via Plan 9's /dev/bintime: reading it yields 8 bytes of
 * big-endian nanoseconds since the Unix epoch. Backs gettimeofday/clock_gettime
 * (what libc++'s <chrono> system/steady clocks call). */
extern long n9_open(const char*, int);
extern long n9_pread(int, void*, long, long long);
extern long n9_close(int);
static unsigned long long n9_nsec(void){
	int fd = (int)n9_open("/dev/bintime", 0 /*OREAD*/);
	if(fd < 0) return 0;
	unsigned char b[8];
	long n = n9_pread(fd, b, 8, -1);
	n9_close(fd);
	if(n < 8) return 0;
	unsigned long long ns = 0; int i;
	for(i=0;i<8;i++) ns = (ns<<8) | b[i];   /* big-endian */
	return ns;
}
int gettimeofday(void *tvp, void *tz){
	(void)tz;
	struct { long tv_sec, tv_usec; } *tv = tvp;
	unsigned long long ns = n9_nsec();
	tv->tv_sec = (long)(ns / 1000000000ULL);
	tv->tv_usec = (long)((ns % 1000000000ULL) / 1000);
	return 0;
}
struct n9_timespec { long tv_sec, tv_nsec; };
int clock_gettime(int clk, void *tsp){
	(void)clk;
	struct n9_timespec *ts = tsp;
	unsigned long long ns = n9_nsec();
	ts->tv_sec = (long)(ns / 1000000000ULL);
	ts->tv_nsec = (long)(ns % 1000000000ULL);
	return 0;
}
long time(long *t){ long s = (long)(n9_nsec()/1000000000ULL); if(t)*t=s; return s; }

/* fenv: cc9 keeps FP traps masked (n9_cli sets the FCR once) and the tests
 * don't inspect IEEE exception flags, so openlibm's fenv hooks are no-ops. */
int feraiseexcept(int e){ (void)e; return 0; }
int feclearexcept(int e){ (void)e; return 0; }
int fetestexcept(int e){ (void)e; return 0; }
int fegetenv(void *p){ (void)p; return 0; }
int fesetenv(const void *p){ (void)p; return 0; }
int feholdexcept(void *p){ (void)p; return 0; }
int feupdateenv(const void *p){ (void)p; return 0; }
int fegetround(void){ return 0 /*FE_TONEAREST*/; }
int fesetround(int r){ (void)r; return 0; }

/* Generic GCC atomic library calls — emitted for atomics that aren't inline
 * lock-free (large/odd-size types). cc9 is single-threaded, so they reduce to
 * plain memory ops; the memory-order arg is irrelevant on a serial machine.
 * The names are builtins, so we define under cc9_ names with __asm__ labels
 * that emit the real __atomic_* symbols (clang won't let us redeclare builtins). */
void cc9_atomic_load(size_t n,const void*src,void*dst,int m) __asm__("__atomic_load");
void cc9_atomic_store(size_t n,void*dst,const void*src,int m) __asm__("__atomic_store");
void cc9_atomic_exchange(size_t n,void*ptr,const void*val,void*ret,int m) __asm__("__atomic_exchange");
int cc9_atomic_cas(size_t n,void*ptr,void*exp,const void*des,int s,int f) __asm__("__atomic_compare_exchange");
void cc9_atomic_load(size_t n,const void*src,void*dst,int m){ (void)m; memcpy(dst,src,n); }
void cc9_atomic_store(size_t n,void*dst,const void*src,int m){ (void)m; memcpy(dst,src,n); }
void cc9_atomic_exchange(size_t n,void*ptr,const void*val,void*ret,int m){ (void)m; memcpy(ret,ptr,n); memcpy(ptr,val,n); }
int cc9_atomic_cas(size_t n,void*ptr,void*exp,const void*des,int s,int f){
	(void)s; (void)f;
	if(memcmp(ptr,exp,n)==0){ memcpy(ptr,des,n); return 1; }
	memcpy(exp,ptr,n); return 0;
}
