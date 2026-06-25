typedef unsigned long size_t;
extern void n9_exits(const char*);
static char arena[1<<20];          /* 1MB bump heap (sbrk-backed later) */
static size_t apos = 0;
void *malloc(size_t n){ n=(n+15)&~(size_t)15; if(apos+n>sizeof arena) return 0; void*p=&arena[apos]; apos+=n; return p; }
void *calloc(size_t a,size_t b){ size_t n=a*b; char*p=malloc(n); if(p) for(size_t i=0;i<n;i++)p[i]=0; return p; }
void *realloc(void*old,size_t n){ char*p=malloc(n); if(p&&old){ char*o=old; for(size_t i=0;i<n;i++)p[i]=o[i]; } return p; }
void free(void*p){ (void)p; }
void abort(void){ n9_exits("cc9: abort\n"); for(;;){} }
void *memcpy(void*d,const void*s,size_t n){ char*a=d; const char*b=s; for(size_t i=0;i<n;i++)a[i]=b[i]; return d; }
void *memmove(void*d,const void*s,size_t n){ char*a=d; const char*b=s; if(a<b)for(size_t i=0;i<n;i++)a[i]=b[i]; else for(size_t i=n;i>0;i--)a[i-1]=b[i-1]; return d; }
void *memset(void*d,int c,size_t n){ char*a=d; for(size_t i=0;i<n;i++)a[i]=(char)c; return d; }
int memcmp(const void*x,const void*y,size_t n){ const unsigned char*a=x,*b=y; for(size_t i=0;i<n;i++) if(a[i]!=b[i]) return a[i]-b[i]; return 0; }
size_t strlen(const char*s){ size_t n=0; while(s[n])n++; return n; }

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
