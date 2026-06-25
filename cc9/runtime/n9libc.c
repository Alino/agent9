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
void *memcpy(void*d,const void*s,size_t n){ char*a=d; const char*b=s; for(size_t i=0;i<n;i++)a[i]=b[i]; return d; }
void *memmove(void*d,const void*s,size_t n){ char*a=d; const char*b=s; if(a<b)for(size_t i=0;i<n;i++)a[i]=b[i]; else for(size_t i=n;i>0;i--)a[i-1]=b[i-1]; return d; }
void *memset(void*d,int c,size_t n){ char*a=d; for(size_t i=0;i<n;i++)a[i]=(char)c; return d; }
int memcmp(const void*x,const void*y,size_t n){ const unsigned char*a=x,*b=y; for(size_t i=0;i<n;i++) if(a[i]!=b[i]) return a[i]-b[i]; return 0; }
size_t strlen(const char*s){ size_t n=0; while(s[n])n++; return n; }

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

/* math: hardware sqrt + simple rounding/abs/fmod (enough for typical compute) */
double sqrt(double x){ double r; __asm__("sqrtsd %1,%0":"=x"(r):"x"(x)); return r; }
float sqrtf(float x){ float r; __asm__("sqrtss %1,%0":"=x"(r):"x"(x)); return r; }
double fabs(double x){ return x<0?-x:x; }
float fabsf(float x){ return x<0?-x:x; }
double trunc(double x){ return (double)(long long)x; }
double floor(double x){ long long t=(long long)x; double d=(double)t; return d>x?d-1:d; }
double ceil(double x){ long long t=(long long)x; double d=(double)t; return d<x?d+1:d; }
double round(double x){ return x<0?ceil(x-0.5):floor(x+0.5); }
double nearbyint(double x){ return round(x); }
double rint(double x){ return round(x); }
float floorf(float x){ return (float)floor(x); }
float ceilf(float x){ return (float)ceil(x); }
float truncf(float x){ return (float)trunc(x); }
double fmod(double a,double b){ if(b==0)return 0; return a-trunc(a/b)*b; }
double copysign(double x,double y){ return y<0?-fabs(x):fabs(x); }
double scalbn(double x,int n){ while(n>0){x*=2;n--;} while(n<0){x*=0.5;n++;} return x; }
double ldexp(double x,int n){ return scalbn(x,n); }
double frexp(double x,int*e){ int n=0; if(x!=0){ double a=fabs(x); while(a>=1){a*=0.5;n++;} while(a<0.5){a*=2;n--;} } *e=n; return scalbn(x,-n); }
