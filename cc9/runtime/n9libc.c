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
