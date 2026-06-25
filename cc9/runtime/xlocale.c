/* cc9 xlocale *_l family — the POSIX extended-locale functions libc++'s
 * locale_base_api expects on a BSD-like platform. cc9 is C-locale only, so each
 * ignores the locale_t and forwards to the base function. */
typedef unsigned long size_t;
typedef void *locale_t;
struct tm;

/* base functions (defined in n9libc / its strftime) */
int isalnum(int),isalpha(int),isblank(int),iscntrl(int),isdigit(int),isgraph(int);
int islower(int),isprint(int),ispunct(int),isspace(int),isupper(int),isxdigit(int);
int toupper(int),tolower(int);
int strcmp(const char*,const char*);
size_t strxfrm(char*,const char*,size_t);
size_t strftime(char*,size_t,const char*,const struct tm*);

#define CT_L(name) int name##_l(int c, locale_t l){ (void)l; return name(c); }
CT_L(isalnum) CT_L(isalpha) CT_L(isblank) CT_L(iscntrl) CT_L(isdigit) CT_L(isgraph)
CT_L(islower) CT_L(isprint) CT_L(ispunct) CT_L(isspace) CT_L(isupper) CT_L(isxdigit)
CT_L(toupper) CT_L(tolower)

int strcoll_l(const char*a,const char*b,locale_t l){ (void)l; return strcmp(a,b); }
size_t strxfrm_l(char*d,const char*s,size_t n,locale_t l){ (void)l; return strxfrm(d,s,n); }
size_t strftime_l(char*s,size_t m,const char*f,const struct tm*t,locale_t l){ (void)l; return strftime(s,m,f,t); }
