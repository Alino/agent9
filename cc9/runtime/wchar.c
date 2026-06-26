/* cc9 wide-character C library. cc9 runs the C locale, so the byte<->wchar
 * mapping is identity over 0..255 (Latin-1) and wide ctype delegates to narrow
 * for ASCII. Enough to back libc++'s wchar_t char_traits, wide facets, and
 * wide streams. */
typedef unsigned long size_t;
typedef __WCHAR_TYPE__ wchar_t;   /* clang's exact wchar_t — matches the C++ side */
typedef int wint_t;
typedef void *locale_t;
#define WEOF ((wint_t)-1)
struct mbstate_t_; /* opaque; the C locale is stateless */

extern int isalnum(int),isalpha(int),isblank(int),iscntrl(int),isdigit(int),isgraph(int);
extern int islower(int),isprint(int),ispunct(int),isspace(int),isupper(int),isxdigit(int);
extern int tolower(int),toupper(int);
extern long strtol(const char*,char**,int); extern unsigned long strtoul(const char*,char**,int);
extern long long strtoll(const char*,char**,int); extern unsigned long long strtoull(const char*,char**,int);
extern double strtod(const char*,char**); extern float strtof(const char*,char**); extern long double strtold(const char*,char**);
extern size_t strftime(char*,size_t,const char*,const void*);

/* ---- wide string / memory ---- */
size_t wcslen(const wchar_t*s){ size_t n=0; while(s[n])n++; return n; }
wchar_t *wcscpy(wchar_t*d,const wchar_t*s){ wchar_t*r=d; while((*d++=*s++)); return r; }
wchar_t *wcsncpy(wchar_t*d,const wchar_t*s,size_t n){ wchar_t*r=d; while(n&&(*d=*s)){d++;s++;n--;} while(n--)*d++=0; return r; }
wchar_t *wcscat(wchar_t*d,const wchar_t*s){ wchar_t*r=d; while(*d)d++; while((*d++=*s++)); return r; }
wchar_t *wcsncat(wchar_t*d,const wchar_t*s,size_t n){ wchar_t*r=d; while(*d)d++; while(n&&*s){*d++=*s++;n--;} *d=0; return r; }
int wcscmp(const wchar_t*a,const wchar_t*b){ while(*a&&*a==*b){a++;b++;} return (int)(*a-*b); }
int wcsncmp(const wchar_t*a,const wchar_t*b,size_t n){ while(n&&*a&&*a==*b){a++;b++;n--;} return n?(int)(*a-*b):0; }
int wcscoll(const wchar_t*a,const wchar_t*b){ return wcscmp(a,b); }
size_t wcsxfrm(wchar_t*d,const wchar_t*s,size_t n){ size_t l=wcslen(s),i; if(n){ for(i=0;i<n-1&&i<l;i++)d[i]=s[i]; d[i<n?i:n-1]=0; } return l; }
wchar_t *wcschr(const wchar_t*s,wchar_t c){ for(;;s++){ if(*s==c)return (wchar_t*)s; if(!*s)return 0; } }
wchar_t *wcsrchr(const wchar_t*s,wchar_t c){ const wchar_t*r=0; for(;;s++){ if(*s==c)r=s; if(!*s)return (wchar_t*)r; } }
wchar_t *wcsstr(const wchar_t*h,const wchar_t*n){ if(!*n)return (wchar_t*)h; for(;*h;h++){ const wchar_t*a=h,*b=n; while(*a&&*b&&*a==*b){a++;b++;} if(!*b)return (wchar_t*)h; } return 0; }
size_t wcsspn(const wchar_t*s,const wchar_t*set){ size_t n=0; for(;s[n];n++){ const wchar_t*p=set; while(*p&&*p!=s[n])p++; if(!*p)break; } return n; }
size_t wcscspn(const wchar_t*s,const wchar_t*set){ size_t n=0; for(;s[n];n++){ const wchar_t*p=set; while(*p&&*p!=s[n])p++; if(*p)break; } return n; }
wchar_t *wcspbrk(const wchar_t*s,const wchar_t*set){ for(;*s;s++){ const wchar_t*p=set; while(*p){ if(*p==*s)return (wchar_t*)s; p++; } } return 0; }
wchar_t *wcstok(wchar_t*s,const wchar_t*sep,wchar_t**save){ if(!s)s=*save; if(!s)return 0; s+=wcsspn(s,sep); if(!*s){*save=0;return 0;} wchar_t*t=s+wcscspn(s,sep); if(*t){*t=0;*save=t+1;}else *save=0; return s; }
wchar_t *wmemcpy(wchar_t*d,const wchar_t*s,size_t n){ for(size_t i=0;i<n;i++)d[i]=s[i]; return d; }
wchar_t *wmemmove(wchar_t*d,const wchar_t*s,size_t n){ if(d<s)for(size_t i=0;i<n;i++)d[i]=s[i]; else for(size_t i=n;i>0;i--)d[i-1]=s[i-1]; return d; }
wchar_t *wmemset(wchar_t*d,wchar_t c,size_t n){ for(size_t i=0;i<n;i++)d[i]=c; return d; }
int wmemcmp(const wchar_t*a,const wchar_t*b,size_t n){ for(size_t i=0;i<n;i++)if(a[i]!=b[i])return a[i]<b[i]?-1:1; return 0; }
wchar_t *wmemchr(const wchar_t*s,wchar_t c,size_t n){ for(size_t i=0;i<n;i++)if(s[i]==c)return (wchar_t*)(s+i); return 0; }

/* ---- conversion (C locale: identity over bytes) ---- */
wint_t btowc(int c){ return (c==-1)?WEOF:(wint_t)(unsigned char)c; }
int wctob(wint_t c){ return (c>=0&&c<256)?(int)c:-1; }
size_t mbrtowc(wchar_t*pwc,const char*s,size_t n,void*st){ (void)st; if(!s)return 0; if(n==0)return (size_t)-2; if(pwc)*pwc=(wchar_t)(unsigned char)*s; return *s?1:0; }
size_t wcrtomb(char*s,wchar_t wc,void*st){ (void)st; if(!s)return 1; *s=(char)wc; return 1; }
size_t mbrlen(const char*s,size_t n,void*st){ (void)st; if(!s||!*s)return 0; return n?1:(size_t)-2; }
int mbsinit(const void*st){ (void)st; return 1; }
size_t mbsrtowcs(wchar_t*d,const char**src,size_t len,void*st){ (void)st; const char*s=*src; size_t i=0; for(;(!d||i<len)&&s[i];i++) if(d)d[i]=(wchar_t)(unsigned char)s[i]; if(d){ if(i<len){d[i]=0;*src=0;}else *src=s+i; } return i; }
size_t wcsrtombs(char*d,const wchar_t**src,size_t len,void*st){ (void)st; const wchar_t*s=*src; size_t i=0; for(;(!d||i<len)&&s[i];i++) if(d)d[i]=(char)s[i]; if(d){ if(i<len){d[i]=0;*src=0;}else *src=s+i; } return i; }
size_t mbsnrtowcs(wchar_t*d,const char**src,size_t nms,size_t len,void*st){ (void)st; const char*s=*src; size_t i=0; for(;i<nms&&(!d||i<len)&&s[i];i++) if(d)d[i]=(wchar_t)(unsigned char)s[i]; if(d)*src=s+i; return i; }
size_t wcsnrtombs(char*d,const wchar_t**src,size_t nwc,size_t len,void*st){ (void)st; const wchar_t*s=*src; size_t i=0; for(;i<nwc&&(!d||i<len)&&s[i];i++) if(d)d[i]=(char)s[i]; if(d)*src=s+i; return i; }

/* ---- <stdlib.h> multibyte (C locale: identity over bytes) ---- */
int mblen(const char*s,size_t n){ (void)n; if(!s||!*s)return 0; return 1; }
int mbtowc(wchar_t*pwc,const char*s,size_t n){ (void)n; if(!s)return 0; if(pwc)*pwc=(wchar_t)(unsigned char)*s; return *s?1:0; }
int wctomb(char*s,wchar_t wc){ if(!s)return 0; *s=(char)wc; return 1; }
size_t mbstowcs(wchar_t*d,const char*s,size_t n){ size_t i=0; for(;(!d||i<n)&&s[i];i++) if(d)d[i]=(wchar_t)(unsigned char)s[i]; if(d&&i<n)d[i]=0; return i; }
size_t wcstombs(char*d,const wchar_t*s,size_t n){ size_t i=0; for(;(!d||i<n)&&s[i];i++) if(d)d[i]=(char)s[i]; if(d&&i<n)d[i]=0; return i; }

/* ---- numeric (narrow the wide string, reuse strto*) ---- */
static void wnarrow(const wchar_t*w,char*b,int n){ int i=0; for(;i<n-1&&w[i];i++)b[i]=(char)w[i]; b[i]=0; }
long wcstol(const wchar_t*s,wchar_t**e,int b){ char buf[128]; wnarrow(s,buf,128); char*ee; long r=strtol(buf,&ee,b); if(e)*e=(wchar_t*)s+(ee-buf); return r; }
unsigned long wcstoul(const wchar_t*s,wchar_t**e,int b){ char buf[128]; wnarrow(s,buf,128); char*ee; unsigned long r=strtoul(buf,&ee,b); if(e)*e=(wchar_t*)s+(ee-buf); return r; }
long long wcstoll(const wchar_t*s,wchar_t**e,int b){ char buf[128]; wnarrow(s,buf,128); char*ee; long long r=strtoll(buf,&ee,b); if(e)*e=(wchar_t*)s+(ee-buf); return r; }
unsigned long long wcstoull(const wchar_t*s,wchar_t**e,int b){ char buf[128]; wnarrow(s,buf,128); char*ee; unsigned long long r=strtoull(buf,&ee,b); if(e)*e=(wchar_t*)s+(ee-buf); return r; }
double wcstod(const wchar_t*s,wchar_t**e){ char buf[128]; wnarrow(s,buf,128); char*ee; double r=strtod(buf,&ee); if(e)*e=(wchar_t*)s+(ee-buf); return r; }
float wcstof(const wchar_t*s,wchar_t**e){ char buf[128]; wnarrow(s,buf,128); char*ee; float r=strtof(buf,&ee); if(e)*e=(wchar_t*)s+(ee-buf); return r; }
long double wcstold(const wchar_t*s,wchar_t**e){ char buf[128]; wnarrow(s,buf,128); char*ee; long double r=strtold(buf,&ee); if(e)*e=(wchar_t*)s+(ee-buf); return r; }

/* ---- wide ctype (ASCII via narrow) ---- */
#define WCT(n) int n##_w(wint_t c){ return (c>=0&&c<128)? n((int)c):0; }
int iswalnum(wint_t c){ return (c>=0&&c<128)?isalnum((int)c):0; }
int iswalpha(wint_t c){ return (c>=0&&c<128)?isalpha((int)c):0; }
int iswblank(wint_t c){ return (c>=0&&c<128)?isblank((int)c):0; }
int iswcntrl(wint_t c){ return (c>=0&&c<128)?iscntrl((int)c):0; }
int iswdigit(wint_t c){ return (c>=0&&c<128)?isdigit((int)c):0; }
int iswgraph(wint_t c){ return (c>=0&&c<128)?isgraph((int)c):0; }
int iswlower(wint_t c){ return (c>=0&&c<128)?islower((int)c):0; }
int iswprint(wint_t c){ return (c>=0&&c<128)?isprint((int)c):0; }
int iswpunct(wint_t c){ return (c>=0&&c<128)?ispunct((int)c):0; }
int iswspace(wint_t c){ return (c>=0&&c<128)?isspace((int)c):0; }
int iswupper(wint_t c){ return (c>=0&&c<128)?isupper((int)c):0; }
int iswxdigit(wint_t c){ return (c>=0&&c<128)?isxdigit((int)c):0; }
wint_t towlower(wint_t c){ return (c>=0&&c<128)?(wint_t)tolower((int)c):c; }
wint_t towupper(wint_t c){ return (c>=0&&c<128)?(wint_t)toupper((int)c):c; }
/* wctype/iswctype by name */
static int wstreq(const char*a,const char*b){ while(*a&&*a==*b){a++;b++;} return *a==*b; }
unsigned long wctype(const char*name){
	const char* names[]={"alnum","alpha","blank","cntrl","digit","graph","lower","print","punct","space","upper","xdigit"};
	for(unsigned long i=0;i<12;i++) if(wstreq(name,names[i])) return i+1; return 0;
}
int iswctype(wint_t c,unsigned long t){
	switch(t){case 1:return iswalnum(c);case 2:return iswalpha(c);case 3:return iswblank(c);case 4:return iswcntrl(c);
	case 5:return iswdigit(c);case 6:return iswgraph(c);case 7:return iswlower(c);case 8:return iswprint(c);
	case 9:return iswpunct(c);case 10:return iswspace(c);case 11:return iswupper(c);case 12:return iswxdigit(c);} return 0;
}
static const int wtrans_lower=1, wtrans_upper=2;
const int *wctrans(const char*name){ if(wstreq(name,"tolower"))return &wtrans_lower; if(wstreq(name,"toupper"))return &wtrans_upper; return 0; }
wint_t towctrans(wint_t c,const int*t){ if(!t)return c; return *t==2?towupper(c):towlower(c); }

/* ---- xlocale wide *_l (ignore locale) ---- */
int iswalnum_l(wint_t c,locale_t l){(void)l;return iswalnum(c);} int iswalpha_l(wint_t c,locale_t l){(void)l;return iswalpha(c);}
int iswblank_l(wint_t c,locale_t l){(void)l;return iswblank(c);} int iswcntrl_l(wint_t c,locale_t l){(void)l;return iswcntrl(c);}
int iswdigit_l(wint_t c,locale_t l){(void)l;return iswdigit(c);} int iswgraph_l(wint_t c,locale_t l){(void)l;return iswgraph(c);}
int iswlower_l(wint_t c,locale_t l){(void)l;return iswlower(c);} int iswprint_l(wint_t c,locale_t l){(void)l;return iswprint(c);}
int iswpunct_l(wint_t c,locale_t l){(void)l;return iswpunct(c);} int iswspace_l(wint_t c,locale_t l){(void)l;return iswspace(c);}
int iswupper_l(wint_t c,locale_t l){(void)l;return iswupper(c);} int iswxdigit_l(wint_t c,locale_t l){(void)l;return iswxdigit(c);}
wint_t towlower_l(wint_t c,locale_t l){(void)l;return towlower(c);} wint_t towupper_l(wint_t c,locale_t l){(void)l;return towupper(c);}
unsigned long wctype_l(const char*n,locale_t l){(void)l;return wctype(n);} int iswctype_l(wint_t c,unsigned long t,locale_t l){(void)l;return iswctype(c,t);}
wint_t towctrans_l(wint_t c,const int*t,locale_t l){(void)l;return towctrans(c,t);} const int *wctrans_l(const char*n,locale_t l){(void)l;return wctrans(n);}
int wcscoll_l(const wchar_t*a,const wchar_t*b,locale_t l){(void)l;return wcscmp(a,b);}
size_t wcsxfrm_l(wchar_t*d,const wchar_t*s,size_t n,locale_t l){(void)l;return wcsxfrm(d,s,n);}

/* ---- wide stdio over the narrow FILE layer ---- */
typedef struct _CC9_FILE FILE;
extern int fgetc(FILE*); extern int fputc(int,FILE*); extern int ungetc(int,FILE*);
wint_t fgetwc(FILE*f){ int c=fgetc(f); return c<0?WEOF:(wint_t)c; }
wint_t getwc(FILE*f){ return fgetwc(f); }
wint_t fputwc(wchar_t c,FILE*f){ return fputc((int)(unsigned char)c,f)<0?WEOF:(wint_t)c; }
wint_t putwc(wchar_t c,FILE*f){ return fputwc(c,f); }
wint_t ungetwc(wint_t c,FILE*f){ return c==WEOF?WEOF:(ungetc((int)c,f)<0?WEOF:c); }
int fwide(FILE*f,int mode){ (void)f; return mode; }
wchar_t *fgetws(wchar_t*s,int n,FILE*f){ int i=0; while(i<n-1){ wint_t c=fgetwc(f); if(c==WEOF){ if(i==0)return 0; break; } s[i++]=(wchar_t)c; if(c==L'\n')break; } s[i]=0; return s; }
int fputws(const wchar_t*s,FILE*f){ while(*s) if(fputwc(*s++,f)==WEOF)return -1; return 0; }

/* swprintf/vswprintf: narrow the format, run vsnprintf, widen (C-locale ASCII).
 * Good enough for the wide num facet's fallbacks and simple user formatting. */
extern int vsnprintf(char*,size_t,const char*,__builtin_va_list);
int vswprintf(wchar_t*ws,size_t n,const wchar_t*wf,__builtin_va_list ap){
	char nf[256]; size_t i; for(i=0;i+1<sizeof nf&&wf[i];i++)nf[i]=(char)wf[i]; nf[i]=0;
	char nb[512]; int r=vsnprintf(nb,sizeof nb,nf,ap);
	if(r<0){ if(n)ws[0]=0; return -1; }
	int have = r < (int)sizeof nb ? r : (int)sizeof nb - 1;   /* bytes actually in nb (avoid OOB read) */
	for(i=0;(int)i<have&&i+1<n;i++)ws[i]=(wchar_t)(unsigned char)nb[i]; if(n)ws[i]=0;
	/* C requires a NEGATIVE return on truncation (unlike snprintf) */
	if((size_t)r>=n || r>=(int)sizeof nb) return -1;
	return r;
}
int swprintf(wchar_t*ws,size_t n,const wchar_t*wf,...){ __builtin_va_list ap; __builtin_va_start(ap,wf); int r=vswprintf(ws,n,wf,ap); __builtin_va_end(ap); return r; }
int swscanf(const wchar_t*s,const wchar_t*f,...){ (void)s;(void)f; return 0; }
size_t wcsftime(wchar_t*ws,size_t n,const wchar_t*wf,const void*tm){
	char nf[128]; size_t i; for(i=0;i+1<sizeof nf&&wf[i];i++)nf[i]=(char)wf[i]; nf[i]=0;
	char nb[256]; size_t r=strftime(nb,sizeof nb,nf,tm);
	if(r==0){ if(n)ws[0]=0; return 0; }                  /* strftime overflowed its own buffer */
	for(i=0;i<r&&i+1<n;i++)ws[i]=(wchar_t)(unsigned char)nb[i];
	if(i<r){ if(n)ws[0]=0; return 0; }                   /* didn't fit ws: C wcsftime returns 0 */
	if(n)ws[i]=0; return i;
}
size_t wcsftime_l(wchar_t*ws,size_t n,const wchar_t*wf,const void*tm,locale_t l){ (void)l; return wcsftime(ws,n,wf,tm); }
