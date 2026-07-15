typedef unsigned long size_t;
extern void n9_exits(const char*);
static char *utoa_(unsigned long long v, char *p, int base);  /* defined below */
double strtod(const char*, char**); float strtof(const char*, char**); long double strtold(const char*, char**);
long strtol(const char*, char**, int); unsigned long strtoul(const char*, char**, int);

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

static void free_u(void *ap){
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
	/* nu*sizeof(Header) must fit in a positive long for n9_sbrk (else the cast
	 * goes negative and shrinks the break / corrupts the heap). */
	if(nu > 0x7fffffffffffffffUL / sizeof(Header)) return 0;
	void *cp = n9_sbrk((long)(nu * sizeof(Header)));
	if(cp == (void*)-1) return 0;
	Header *up = (Header*)cp; up->s.size = nu;
	free_u((void*)(up + 1));
	return kr_freep;
}
static void *malloc_u(size_t nbytes){
	if(nbytes == 0) return 0;
	/* guard the nunits round-up + the +1 header against size_t overflow */
	if(nbytes > (size_t)-1 - 2*sizeof(Header)) return 0;
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
/* Public allocator: a Plan 9 semaphore serializes the shared heap across threads
 * (cc9 threads share memory via rfork(RFMEM)). Single-threaded programs pay one
 * uncontended semacquire/semrelease per call. */
extern int n9_semacquire(int *, int);
extern int n9_semrelease(int *, int);
static int malloc_lock = 1;
#ifdef CC9_RECURSE_PROBE
/* DEBUG: malloc is THE allocator (clang's libc++ operator new calls it). If hit
 * with the stack already deep (runaway recursion), walk our frame chain and dump
 * return addresses to fd 2, then exit — catches the recursion cycle in-process. */
extern char __cc9_main_stack[];
extern long n9_pwrite(int, const void *, long, long long);
extern void n9_exits(const char *);
static int cc9_probe_armed = 1;
static void cc9_dump_chain_malloc(void){
	cc9_probe_armed = 0;
	n9_pwrite(2, "CC9-RECURSE-CHAIN:\n", 19, -1);
	void **fp = (void **)__builtin_frame_address(0);
	for (int i = 0; i < 60 && fp; i++){
		void *ret = fp[1];
		char b[20]; int k = 0; b[k++]='0'; b[k++]='x';
		unsigned long v = (unsigned long)ret;
		for (int j = 15; j >= 0; j--){ int d = (v>>(j*4))&0xf; b[k++] = d<10?'0'+d:'a'+d-10; }
		b[k++]='\n'; n9_pwrite(2, b, k, -1);
		void **nx = (void **)fp[0];
		if (nx <= fp) break;
		fp = nx;
	}
	n9_exits("cc9-recurse");
}
#endif
void *malloc(size_t n){
#ifdef CC9_RECURSE_PROBE
	{ char probe; if (cc9_probe_armed && (unsigned long)&probe < (unsigned long)__cc9_main_stack + (unsigned long)CC9_STACK_BYTES - 64UL*1024*1024) cc9_dump_chain_malloc(); }
#endif
	n9_semacquire(&malloc_lock,1); void *r=malloc_u(n); n9_semrelease(&malloc_lock,1); return r; }
void free(void *p){
#ifdef CC9_RECURSE_PROBE
	{ char probe; if (cc9_probe_armed && (unsigned long)&probe < (unsigned long)__cc9_main_stack + (unsigned long)CC9_STACK_BYTES - 64UL*1024*1024) cc9_dump_chain_malloc(); }
#endif
	if(!p) return; n9_semacquire(&malloc_lock,1); free_u(p); n9_semrelease(&malloc_lock,1); }
/* malloc_usable_size(p) — the real allocated size of a block, which is >= what
 * was asked for (the request is rounded up to whole Header units).
 *
 * Worth doing exactly rather than returning 0: SpiderMonkey feeds this into its
 * GC pressure accounting (MallocSizeOf in mozjs's jsglue.cpp), so a stubbed
 * answer would not fail loudly — it would just quietly mistime collections.
 *
 * Mirrors free_u's two cases: a plain block sits one Header below the pointer,
 * and an aligned_alloc block stamps CC9_ALIGN_MAGIC at ap[-1] with the real
 * malloc base at ap[-2]. For the aligned case we report the usable bytes from
 * the RETURNED pointer to the end of the underlying block — not the whole
 * block — since the alignment padding ahead of it is not the caller's to use. */
size_t malloc_usable_size(void *ap){
	if(!ap) return 0;
	char *base = (char *)ap;
	if(((unsigned long*)ap)[-1] == CC9_ALIGN_MAGIC) base = *(char **)((char *)ap - 16);
	Header *bp = (Header*)base - 1;
	size_t total = bp->s.size * sizeof(Header);        /* header + payload */
	size_t used  = (size_t)((char *)ap - (char *)bp);  /* header + any align pad */
	return total > used ? total - used : 0;
}

/* aligned_alloc(alignment, size) (C11). malloc is 16-byte aligned, so small
 * alignments are free; larger ones over-allocate and align, recording the base
 * + sentinel so plain free() reclaims it (see free above). */
void *aligned_alloc(size_t al, size_t size){
	if(al <= 16) return malloc(size);
	if(al & (al - 1)) return 0;                    /* alignment must be a power of two */
	if(size > (size_t)-1 - al - 16) return 0;      /* size+al+16 overflow guard */
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
void *calloc(size_t a,size_t b){
	if(a && b > (size_t)-1 / a) return 0;            /* multiply-overflow guard */
	size_t n=a*b; char*p=malloc(n); if(p) for(size_t i=0;i<n;i++)p[i]=0; return p;
}
void *realloc(void*old,size_t n){
	if(!old) return malloc(n);
	if(n==0){ free(old); return 0; }
	/* An aligned_alloc'd pointer carries the sentinel at old[-1] and its real
	 * malloc base at old[-2]; size/free must use the base, not old-1. */
	void *base = old; size_t oldsz;
	if(((unsigned long*)old)[-1] == CC9_ALIGN_MAGIC){
		base = *(void**)((char*)old - 16);
		Header *bp=(Header*)base-1; size_t blk=(bp->s.size-1)*sizeof(Header);
		oldsz = blk - (size_t)((char*)old - (char*)base);   /* usable bytes from `old` */
	} else {
		Header *bp=(Header*)old-1; oldsz=(bp->s.size-1)*sizeof(Header);
	}
	n9_semacquire(&malloc_lock,1);
	char *np=malloc_u(n);
	if(np){ size_t c=oldsz<n?oldsz:n, i; char*o=old; for(i=0;i<c;i++) np[i]=o[i]; free_u(base); }
	n9_semrelease(&malloc_lock,1);
	return np;
}
/* abort(): raise SIGABRT first so a user-installed handler runs (POSIX), then
 * terminate. raise() (below) invokes n9_sigtab[SIGABRT] if set. */
extern int raise(int);
void abort(void){ raise(6 /*SIGABRT*/); n9_exits("cc9: abort\n"); for(;;){} }

/* -finstrument-functions hooks (CC9_RECURSE_PROBE clang build): every instrumented
 * function entry/exit calls these. Track call depth; when it runs away (infinite
 * recursion), dump the deepest call-chain (this_fn addresses) to /tmp/cc9fault and
 * exit — the repeating addresses map (via clang-22.elf) to the recursing functions.
 * Marked no_instrument_function so the hooks don't instrument themselves. */
extern long n9_create(const char *, int, int);
extern long n9_pwrite(int, const void *, long, long long);
#define CC9_RING 256
static void *cc9_ring[CC9_RING];
static unsigned long cc9_sp[CC9_RING];
static int cc9_depth;
__attribute__((no_instrument_function))
static void cc9_hexw(int fd, unsigned long v){ char b[19]; int k=0; b[k++]='0'; b[k++]='x'; for(int j=15;j>=0;j--){int d=(v>>(j*4))&0xf; b[k++]=d<10?'0'+d:'a'+d-10;} b[k++]='\n'; n9_pwrite(fd,b,k,-1); }
extern char __cc9_main_stack[];
#ifndef CC9_STACK_BYTES
#define CC9_STACK_BYTES 268435456
#endif
static volatile int cc9_dumped;
static int cc9_lastfd = -2;
__attribute__((no_instrument_function))
void __cyg_profile_func_enter(void *this_fn, void *call_site){
	(void)call_site;
	unsigned long sp = (unsigned long)__builtin_frame_address(0);
	int slot = cc9_depth++ & (CC9_RING-1);
	cc9_ring[slot] = this_fn; cc9_sp[slot] = sp;
	unsigned long top0 = (unsigned long)__cc9_main_stack + (unsigned long)CC9_STACK_BYTES;
	/* Once the stack is already deep (>1MB used), record each function to /tmp/cc9last
	 * (overwrite). Cheap (shallow calls skip it); after an uncatchable crash the file
	 * names the last deep function — close to the culprit. */
	if((top0 - sp) > 1024UL*1024){
		if(cc9_lastfd == -2) cc9_lastfd = n9_create("/tmp/cc9last", 1, 0666);
		if(cc9_lastfd >= 0){
			char b[40]; int k=0; b[k++]='f'; b[k++]='=';
			for(int j=15;j>=0;j--){int d=((unsigned long)this_fn>>(j*4))&0xf; b[k++]=d<10?'0'+d:'a'+d-10;}
			b[k++]=' '; b[k++]='s'; b[k++]='p'; b[k++]='=';
			for(int j=15;j>=0;j--){int d=(sp>>(j*4))&0xf; b[k++]=d<10?'0'+d:'a'+d-10;}
			b[k++]='\n'; n9_pwrite(cc9_lastfd, b, k, 0);
		}
	}
	/* trigger on STACK USAGE (catches huge-frame recursion). Stack grows down from
	 * __cc9_main_stack + CC9_STACK_BYTES; dump when >50MB used. Dump pairs (this_fn, sp)
	 * so monotonic sp growth proves recursion and identifies the growing function. */
	unsigned long top = (unsigned long)__cc9_main_stack + (unsigned long)CC9_STACK_BYTES;
	if(!cc9_dumped && (top - sp) > 50UL*1024*1024){
		cc9_dumped = 1;
		int fd = n9_create("/tmp/cc9fault", 1, 0666);
		if(fd>=0){
			n9_pwrite(fd,"CC9 RUNAWAY (stack>50MB); recent (this_fn sp):\n",47,-1);
			for(int i=0;i<CC9_RING;i++){ int s=(cc9_depth+i)&(CC9_RING-1); if(cc9_ring[s]){ cc9_hexw(fd,(unsigned long)cc9_ring[s]); cc9_hexw(fd,cc9_sp[s]); } }
		}
		n9_exits("cc9-recurse-dump");
	}
}
__attribute__((no_instrument_function))
void __cyg_profile_func_exit(void *this_fn, void *call_site){ (void)this_fn; (void)call_site; if(cc9_depth>0) cc9_depth--; }
/* assert support: print the failing expression + file:line to fd 2 before dying,
 * so a built-with-assertions clang names the broken invariant (assert.h routes
 * the standard assert macro here). __assert_fail is the glibc-style entry LLVM may
 * also reference directly. */
extern long n9_pwrite(int, const void *, long, long long);
extern long n9_create(const char *, int, int);
size_t strlen(const char *);
static void cc9_assert_emit(int fd, const char *e, const char *f, const char *ln, int k){
	n9_pwrite(fd,"cc9 assert: ",12,-1); n9_pwrite(fd,e,(long)strlen(e),-1);
	n9_pwrite(fd," at ",4,-1); n9_pwrite(fd,f,(long)strlen(f),-1);
	n9_pwrite(fd,":",1,-1); n9_pwrite(fd,ln,k,-1); n9_pwrite(fd,"\n",1,-1);
}
void __cc9_assert(const char *e, const char *f, int line){
	char ln[16]; int k=0; if(line<=0){ln[k++]='0';} else { char t[16]; int j=0; while(line){t[j++]='0'+line%10;line/=10;} while(j)ln[k++]=t[--j]; }
	cc9_assert_emit(2, e, f, ln, k);                          /* fd 2 (dev VM connection) */
	int ffd = n9_create("/tmp/cc9fault", 1 /*OWRITE*/, 0666); /* file (survives Shuttle conn death) */
	if(ffd>=0) cc9_assert_emit(ffd, e, f, ln, k);
	abort();
}
void __assert_fail(const char *e,const char *f,unsigned line,const char *fn){ (void)fn; __cc9_assert(e,f,(int)line); }
void __assert(const char *e,const char *f,int line){ __cc9_assert(e,f,line); }
void __assert_rtn(const char *fn,const char *f,int line,const char *e){ (void)fn; __cc9_assert(e,f,line); }
/* Encode the numeric exit code in the Plan 9 exits() string so a POSIX waitpid()
 * in the parent can recover it via WEXITSTATUS — the libc++ death-test harness
 * exits the child with an enum code and reads it back. Plan 9 reports a non-empty
 * exit status to the parent's await as "<argv0> <pid>: <string>", so we tag the
 * code with a distinctive marker ("cc9exit=N") that waitpid scans for, immune to
 * that prefix wrapping. 0 maps to the empty status string (Plan 9 clean exit). */
const char *cc9_exitstr(int code){
	if(code==0) return 0;
	static char buf[24]; const char *tag="cc9exit=";
	int k=0; while(tag[k]){ buf[k]=tag[k]; k++; }
	unsigned v = code<0 ? (unsigned)(-code) : (unsigned)code;
	if(code<0) buf[k++]='-';
	char d[12]; int j=0; if(v==0) d[j++]='0'; while(v){ d[j++]='0'+v%10; v/=10; }
	while(j) buf[k++]=d[--j];
	buf[k]=0;
	return buf;
}
/* through crt0's common epilogue: atexit handlers + cc9_kill_threads — a
 * direct n9_exits here orphans worker threads (they'd keep reading fd 0 and
 * steal the parent shell's input). */
void exit(int code){ extern void cc9_exit_common(const char *); cc9_exit_common(cc9_exitstr(code)); for(;;){} }
void _Exit(int code){ exit(code); }
void quick_exit(int code){ exit(code); }
int at_quick_exit(void (*f)(void)){ (void)f; return 0; }   /* no handler table */

/* C signal surface (no POSIX delivery on cc9): track a handler table so
 * signal()/raise() round-trip for the conformance tests; raise runs the
 * handler if one is installed. */
typedef void (*n9_sigh)(int);
static n9_sigh n9_sigtab[32];
n9_sigh signal(int s, n9_sigh h){ if(s<0||s>=32) return (n9_sigh)-1; n9_sigh o=n9_sigtab[s]; n9_sigtab[s]=h; return o; }
int raise(int s){ if(s<0||s>=32) return -1; n9_sigh h=n9_sigtab[s]; if(h&&h!=(n9_sigh)1) h(s); return 0; }
/* register a handler (used by sigaction in posix_llvm.c, which can't see the
 * static table) */
void cc9_set_sigh(int s, n9_sigh h){ if(s>=0&&s<32) n9_sigtab[s]=h; }
/* crt0's note dispatch asks whether a signal has a real handler (SIG_IGN=1
 * counts as handled — the note is then swallowed like an ignored signal). */
int cc9_sig_has_handler(int s){ return s>=0 && s<32 && n9_sigtab[s] != 0; }
/* run the SIGALRM (14) handler from the note handler when a Plan 9 "alarm" note
 * fires (setitimer/alarm). Called in note context; the conformance use is an
 * empty handler, but a real one runs here as the closest signal analogue. */
void cc9_run_sigalrm(void){ n9_sigh h=n9_sigtab[14]; if(h&&h!=(n9_sigh)1&&h!=(n9_sigh)-1) h(14); }
/* __atomic_is_lock_free(size, ptr): native scalar widths (<=16B) are lock-free
 * on amd64. Builtin name, so define via an __asm__ label (see __atomic_* below). */
int cc9_atomic_is_lock_free(size_t n, const volatile void *p) __asm__("__atomic_is_lock_free");
int cc9_atomic_is_lock_free(size_t n, const volatile void *p){ (void)p; return n<=16; }
void *memcpy(void*d,const void*s,size_t n){
#ifdef CC9_RECURSE_PROBE
	{ char probe; if (cc9_probe_armed && (unsigned long)&probe < (unsigned long)__cc9_main_stack + (unsigned long)CC9_STACK_BYTES - 64UL*1024*1024) cc9_dump_chain_malloc(); }
#endif
	char*a=d; const char*b=s; for(size_t i=0;i<n;i++)a[i]=b[i]; return d; }
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
	/* Everything the runtime can actually RETURN needs a name here, or callers
	 * surface "Unknown error 39" to users for an error we chose deliberately.
	 * These are the rest of what fs.c's errstr table and net9.c hand back. */
	case 18: m="Invalid cross-device link"; break; case 25: m="Inappropriate ioctl for device"; break;
	case 36: m="File name too long"; break;        case 38: m="Function not implemented"; break;
	case 39: m="Directory not empty"; break;       case 40: m="Too many levels of symbolic links"; break;
	case 88: m="Socket operation on non-socket"; break;
	case 89: m="Destination address required"; break;
	case 90: m="Message too long"; break;          case 93: m="Protocol not supported"; break;
	case 95: m="Operation not supported"; break;   case 97: m="Address family not supported by protocol"; break;
	case 98: m="Address already in use"; break;    case 104: m="Connection reset by peer"; break;
	case 106: m="Transport endpoint is already connected"; break;
	case 107: m="Transport endpoint is not connected"; break;
	case 110: m="Connection timed out"; break;     case 111: m="Connection refused"; break;
	case 113: m="No route to host"; break;         case 115: m="Operation now in progress"; break;
	}
	/* Return the literal directly: it outlives any caller, needs no copy, and —
	 * unlike the shared `buf` — cannot be torn by another thread calling strerror
	 * at the same time. `buf` is only for the unknown-errno fallback, which is the
	 * one case that has to be formatted. (The copy this replaces was also
	 * unbounded, and several names below are longer than buf.) */
	if(m) return (char *)m;
	char *d=buf; const char*p="Unknown error "; while(*p)*d++=*p++;
	d=utoa_((unsigned long long)(e<0?-e:e), d, 10); *d=0; return buf;
}
/* POSIX/XSI strerror_r (threads-on system_error uses the reentrant form). */
int strerror_r(int e, char *buf, size_t n){ char *m=strerror(e); size_t i; for(i=0;i+1<n && m[i]; i++) buf[i]=m[i]; if(n) buf[i]=0; return 0; }

/* errno + a C locale (always "C": '.' decimal point) for libc++/json.
 * errno is PER-THREAD when the thread runtime is linked, global otherwise.
 * `__thread` was tried and reverted: -femulated-tls turns each access into
 * __emutls_get_address(), dragging pthread.o into EVERY link (n9link chokes).
 * Instead pthread.c provides a WEAK-referenced cc9_thread_errno_slot() that
 * resolves the calling thread's slot by %rsp stack-range (same trick as
 * cur_pid) — thread-free programs leave the symbol unsatisfied (0) and use
 * the global; threaded programs get real per-thread errno with no emutls. */
extern int *cc9_thread_errno_slot(void) __attribute__((weak));
static int n9_errno_v = 0;
int *__n9_errno(void){
	if(cc9_thread_errno_slot){ int *p = cc9_thread_errno_slot(); if(p) return p; }
	return &n9_errno_v;
}
struct lconv { char *decimal_point, *thousands_sep, *grouping, *p[7]; char c[24]; };
static char dot[] = ".";
static char empty[] = "";
static struct lconv n9_lconv = { dot, empty, empty, {empty,empty,empty,empty,empty,empty,empty} };
struct lconv *localeconv(void){ return &n9_lconv; }
char *setlocale(int c, const char *l){ (void)c; (void)l; return (char*)"C"; }
/* minimal strftime for the time_put facet (common specifiers, C locale). */
struct n9_tm { int tm_sec,tm_min,tm_hour,tm_mday,tm_mon,tm_year,tm_wday,tm_yday,tm_isdst; };
static const char *n9_wday[]={"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
static const char *n9_mon[]={"January","February","March","April","May","June","July","August","September","October","November","December"};
/* ISO-8601 week number (1..53) + week-based year, for %V/%G/%g. The 52-vs-53
 * decision uses the standard "Jan 1 weekday" rule (Thu, or Wed in a leap year). */
static int n9_isoyearweeks(int y){
	int p=(y+y/4-y/100+y/400)%7, p1=((y-1)+(y-1)/4-(y-1)/100+(y-1)/400)%7;
	return (p==4||p1==3)?53:52;
}
static int n9_isoweek(const struct n9_tm *t, int *isoyear){
	int y=t->tm_year+1900, wday=(t->tm_wday+6)%7;   /* Mon=0..Sun=6 */
	int week=(t->tm_yday-wday+10)/7;
	if(week<1){ y--; week=n9_isoyearweeks(y); }
	else if(week>52){ int w=n9_isoyearweeks(y); if(week>w){ week=1; y++; } }
	*isoyear=y; return week;
}
size_t strftime(char *s, size_t max, const char *f, const struct n9_tm *t){
	char *o=s, *end=s+max;
	#define PUT(c) do{ if(o<end-1)*o++=(c); }while(0)
	#define PUTS(str) do{ const char*p=(str); while(*p)PUT(*p++); }while(0)
	#define PUT2(n) do{ PUT('0'+((n)/10)%10); PUT('0'+(n)%10); }while(0)
	for(; *f; f++){
		if(*f!='%'){ PUT(*f); continue; }
		f++;
		if(*f=='E'||*f=='O') f++;   /* C locale: alt-format modifiers are no-ops */
		switch(*f){
		case 'Y':{ int y=t->tm_year+1900; char b[8]; int i=0; if(y==0)b[i++]='0'; while(y){b[i++]='0'+y%10;y/=10;} while(i)PUT(b[--i]); }break;
		case 'C': PUT2((t->tm_year+1900)/100); break;
		case 'D': case 'x':{ char b[16]; strftime(b,sizeof b,"%m/%d/%y",t); PUTS(b); }break;
		case 'F':{ char b[16]; strftime(b,sizeof b,"%Y-%m-%d",t); PUTS(b); }break;
		case 'T': case 'X':{ char b[16]; strftime(b,sizeof b,"%H:%M:%S",t); PUTS(b); }break;
		case 'R':{ char b[8]; strftime(b,sizeof b,"%H:%M",t); PUTS(b); }break;
		case 'c':{ char b[32]; strftime(b,sizeof b,"%a %b %e %H:%M:%S %Y",t); PUTS(b); }break;
		case 'r':{ char b[16]; strftime(b,sizeof b,"%I:%M:%S %p",t); PUTS(b); }break;
		case 'u': PUT('0'+(t->tm_wday==0?7:t->tm_wday)); break;
		case 'U': PUT2((t->tm_yday+7-t->tm_wday)/7); break;
		case 'W': PUT2((t->tm_yday+7-(t->tm_wday==0?6:t->tm_wday-1))/7); break;
		case 'V':{ int wy; PUT2(n9_isoweek(t,&wy)); }break;
		case 'G':{ int wy; n9_isoweek(t,&wy); char b[8]; int i=0; int yy=wy<0?-wy:wy; if(!yy)b[i++]='0'; while(yy){b[i++]='0'+yy%10;yy/=10;} if(wy<0)PUT('-'); while(i)PUT(b[--i]); }break;
		case 'g':{ int wy; n9_isoweek(t,&wy); PUT2(wy%100); }break;
		case 'y': PUT2((t->tm_year+1900)%100); break;
		case 'm': PUT2(t->tm_mon+1); break;
		case 'd': PUT2(t->tm_mday); break;
		case 'e': if(t->tm_mday<10)PUT(' '); else PUT('0'+t->tm_mday/10); PUT('0'+t->tm_mday%10); break;
		case 'H': PUT2(t->tm_hour); break;
		case 'I':{ int h=t->tm_hour%12; if(!h)h=12; PUT2(h); }break;
		case 'M': PUT2(t->tm_min); break;
		case 'S': PUT2(t->tm_sec); break;
		case 'p': PUTS(t->tm_hour<12?"AM":"PM"); break;
		case 'j': PUT('0'+(t->tm_yday+1)/100%10); PUT2((t->tm_yday+1)%100); break;
		case 'a': if((unsigned)t->tm_wday<7){ const char*w=n9_wday[t->tm_wday]; PUT(w[0]);PUT(w[1]);PUT(w[2]); } break;
		case 'A': if((unsigned)t->tm_wday<7) PUTS(n9_wday[t->tm_wday]); break;
		case 'b': case 'h': if((unsigned)t->tm_mon<12){ const char*m=n9_mon[t->tm_mon]; PUT(m[0]);PUT(m[1]);PUT(m[2]); } break;
		case 'B': if((unsigned)t->tm_mon<12) PUTS(n9_mon[t->tm_mon]); break;
		case 'w': PUT('0'+t->tm_wday); break;
		case 'n': PUT('\n'); break;
		case 't': PUT('\t'); break;
		case '%': PUT('%'); break;
		default: PUT('%'); if(*f)PUT(*f); break;
		}
		if(!*f) break;
	}
	#undef PUT
	#undef PUTS
	#undef PUT2
	if(max) *o=0;
	return (size_t)(o-s);
}

/* xlocale shim: cc9 is C-locale only, so locale_t is a dummy handle and the
 * *_l functions ignore it. A single static object backs every locale_t. */
static int n9_cloc;
void *newlocale(int m, const char *name, void *base){ (void)m; (void)name; (void)base; return &n9_cloc; }
void *duplocale(void *l){ (void)l; return &n9_cloc; }
void freelocale(void *l){ (void)l; }
void *uselocale(void *l){ (void)l; return &n9_cloc; }
double strtod_l(const char *s, char **e, void *l){ (void)l; return strtod(s,e); }
float strtof_l(const char *s, char **e, void *l){ (void)l; return strtof(s,e); }
long double strtold_l(const char *s, char **e, void *l){ (void)l; return strtold(s,e); }
long strtol_l(const char *s, char **e, int b, void *l){ (void)l; return strtol(s,e,b); }
unsigned long strtoul_l(const char *s, char **e, int b, void *l){ (void)l; return strtoul(s,e,b); }
/* shared integer parser: skip whitespace, optional +/-, 0x/0 base autodetect,
 * honor `base`. *neg set on '-'. endptr per C (== nptr if no digits consumed). */
static unsigned long long n9_ustrto(const char *nptr, char **e, int base, int *neg){
	const char *s = nptr;
	while(*s==' '||*s=='\t'||*s=='\n'||*s=='\r'||*s=='\v'||*s=='\f') s++;
	*neg = 0; if(*s=='+') s++; else if(*s=='-'){ *neg=1; s++; }
	if((base==0||base==16) && s[0]=='0' && (s[1]=='x'||s[1]=='X') &&
	   ((s[2]>='0'&&s[2]<='9')||((s[2]|32)>='a'&&(s[2]|32)<='f'))){ s+=2; base=16; }
	else if(base==0 && s[0]=='0'){ base=8; }
	else if(base==0) base=10;
	unsigned long long r=0; int any=0, ov=0;
	for(;; s++){
		unsigned char c=*s; int d;
		if(c>='0'&&c<='9') d=c-'0';
		else if((c|32)>='a'&&(c|32)<='z') d=(c|32)-'a'+10;
		else break;
		if(d>=base) break;
		if(r > (~0ULL - (unsigned)d)/(unsigned)base) ov=1;
		r = r*(unsigned)base + (unsigned)d; any=1;
	}
	if(e) *e=(char*)(any?s:nptr);
	/* ponytail: flags unsigned-range overflow only (covers every stoul/stoull/stol/
	 * stoll overflow test). Add a per-wrapper signed-range clamp if one ever needs it. */
	if(ov){ n9_errno_v = 34; /* ERANGE */ r = ~0ULL; }
	return r;
}
/* openlibm helpers (libcc9m.a) for the correctly-rounded float parser below. */
long double powl(long double, long double);
long double scalbnl(long double, int);
long double fmal(long double, long double, long double);
/* nexttowardl(x,y) with both args long double is identical to nextafterl;
 * openlibm ships nextafterl but not nexttowardl. */
extern long double nextafterl(long double, long double);
long double nexttowardl(long double x, long double y){ return nextafterl(x, y); }

/* Correctly-rounded decimal scaling: mant * 10^exp10. 10^0..10^27 are exact in
 * 80-bit long double, so the power is accumulated from exact chunks with Dekker
 * two-product error tracking into a double-double, then applied with a single
 * rounded multiply/divide — correctly rounded across the whole long-double range
 * (a plain powl() was off-by-ULP for large exponents, e.g. 2e300).
 * ponytail: when |exp10| > 4932 the power itself overflows LDBL_MAX, so we return
 * 0 for tiny magnitudes even where the quotient would be a representable subnormal
 * (same as the old powl path); the denormal boundary would need a bignum. */
static long double n9_exp10x(int k){ long double r=1.0L; while(k-->0) r*=10.0L; return r; } /* exact, k<=27 */
static long double n9_scale10(unsigned long long mant, int exp10){
	long double m=(long double)mant;
	if(exp10==0) return m;
	int n = exp10<0 ? -exp10 : exp10;
	long double Phi=1.0L, Plo=0.0L;                 /* double-double 10^n */
	while(n>0){ int k=n>27?27:n; long double b=n9_exp10x(k);
		long double p=Phi*b;
		if(__builtin_isinf(p)) return exp10>0 ? p : 0.0L;   /* 10^|exp| overflowed */
		long double e=fmal(Phi,b,-p)+Plo*b; Phi=p+e; Plo=e-(Phi-p); n-=k; }
	if(exp10>0){ long double p=m*Phi; if(!__builtin_isinf(p)) p+=fmal(m,Phi,-p)+m*Plo; return p; }
	long double q=m/Phi; if(q==0.0L) return q;          /* underflow to zero */
	long double ph=q*Phi, pl=fmal(q,Phi,-ph)+q*Plo, r=(m-ph)-pl;
	return q + r/Phi;
}

/* Shared float-magnitude parser for strtod/strtold (no sign, no inf/nan).
 * Handles hex floats (0x<nibbles>.<nibbles>p±N) and decimal: up to 19 significant
 * digits accumulated into a uint64, scaled by n9_scale10 (correctly rounded). *e
 * is set past the consumed number; *any if a number was read; *nz if its
 * significand was nonzero (callers use that to flag underflow-to-zero as ERANGE).
 * ponytail: the significand is truncated to 19 digits before scaling, so inputs
 * with >19 significant digits that land near a rounding halfway point can miss
 * the last ULP; upgrade to a big-int parse only if a libc++ test needs it. */
static long double n9_pflt(const char *s, char **e, int *any, int *nz){
	*any=0; *nz=0;
	/* hex float: 0x must be followed by a hex digit (or '.'+hex digit). */
	if(s[0]=='0' && (s[1]|32)=='x' &&
	   ( ((s[2]>='0'&&s[2]<='9')||((s[2]|32)>='a'&&(s[2]|32)<='f'))
	     || (s[2]=='.' && ((s[3]>='0'&&s[3]<='9')||((s[3]|32)>='a'&&(s[3]|32)<='f'))) )){
		const char *p=s+2; long double m=0; int bexp=0, dot=0, ax=0, nzx=0;
		for(;;p++){
			unsigned char c=*p; int d;
			if(c>='0'&&c<='9') d=c-'0';
			else if((c|32)>='a'&&(c|32)<='f') d=(c|32)-'a'+10;
			else if(c=='.'&&!dot){ dot=1; continue; }
			else break;
			ax=1; if(d) nzx=1;
			m=m*16+d; if(dot) bexp-=4;
		}
		int pexp=0;
		if((*p|32)=='p'){
			const char *ps=p; p++;
			int pn=0; if(*p=='+')p++; else if(*p=='-'){pn=1;p++;}
			if(*p>='0'&&*p<='9'){ int pe=0; while(*p>='0'&&*p<='9'){ if(pe<100000)pe=pe*10+(*p-'0'); p++; } pexp=pn?-pe:pe; }
			else p=ps;
		}
		*e=(char*)p; *any=ax; *nz=nzx;
		return scalbnl(m, bexp+pexp);
	}
	/* decimal */
	const char *p=s; unsigned long long mant=0; int exp10=0, sig=0, dot=0, ax=0, nzd=0;
	for(;;p++){
		unsigned char c=*p;
		if(c>='0'&&c<='9'){
			ax=1; int d=c-'0'; if(d) nzd=1;
			if(sig<19 && (mant!=0 || d!=0)){ mant=mant*10+(unsigned)d; sig++; if(dot)exp10--; }
			else if(mant!=0 || d!=0){ if(!dot)exp10++; }   /* digit past the 19-sig budget */
			else if(dot) exp10--;                          /* leading fractional zero */
		} else if(c=='.'&&!dot){ dot=1; }
		else break;
	}
	if(ax && (*p|32)=='e'){
		const char *es=p; p++;
		int en=0; if(*p=='+')p++; else if(*p=='-'){en=1;p++;}
		if(*p>='0'&&*p<='9'){ int pe=0; while(*p>='0'&&*p<='9'){ if(pe<100000)pe=pe*10+(*p-'0'); p++; } exp10 += en?-pe:pe; }
		else p=es;   /* lone 'e': not part of the number */
	}
	*e=(char*)p; *any=ax; *nz=nzd;
	return n9_scale10(mant, exp10);
}
double strtod(const char *nptr, char **e){
	const char *s=nptr;
	while(*s==' '||*s=='\t'||*s=='\n'||*s=='\r'||*s=='\v'||*s=='\f') s++;
	int neg=0; if(*s=='+')s++; else if(*s=='-'){neg=1;s++;}
	if(((s[0]|32)=='i')&&((s[1]|32)=='n')&&((s[2]|32)=='f')){
		s+=3;
		if(((s[0]|32)=='i')&&((s[1]|32)=='n')&&((s[2]|32)=='i')&&((s[3]|32)=='t')&&((s[4]|32)=='y')) s+=5;
		if(e)*e=(char*)s;
		return neg?-__builtin_inf():__builtin_inf();
	}
	if(((s[0]|32)=='n')&&((s[1]|32)=='a')&&((s[2]|32)=='n')){
		s+=3;
		if(*s=='('){ const char*q=s+1; while(*q&&*q!=')')q++; if(*q==')')s=q+1; }
		if(e)*e=(char*)s;
		return __builtin_nan("");
	}
	char *end; int any, nz; long double v=n9_pflt(s,&end,&any,&nz);
	if(!any){ if(e)*e=(char*)nptr; return neg?-0.0:0.0; }
	if(e)*e=end;
	double d=(double)v;
	if(__builtin_isinf(d)) n9_errno_v = 34;            /* overflow -> ERANGE */
	else if(nz && d==0.0) n9_errno_v = 34;             /* underflow -> ERANGE */
	return neg?-d:d;
}
long strtol(const char *s, char **e, int b){ int neg; unsigned long long r=n9_ustrto(s,e,b,&neg); return neg?-(long)r:(long)r; }

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
/* native long-double parser (NOT a cast from double, so the 64-bit mantissa is
 * kept) — shares n9_pflt with strtod; only the range check is long-double-wide. */
long double strtold(const char *nptr, char **e){
	const char *s=nptr;
	while(*s==' '||*s=='\t'||*s=='\n'||*s=='\r'||*s=='\v'||*s=='\f') s++;
	int neg=0; if(*s=='+')s++; else if(*s=='-'){neg=1;s++;}
	if(((s[0]|32)=='i')&&((s[1]|32)=='n')&&((s[2]|32)=='f')){
		s+=3;
		if(((s[0]|32)=='i')&&((s[1]|32)=='n')&&((s[2]|32)=='i')&&((s[3]|32)=='t')&&((s[4]|32)=='y')) s+=5;
		if(e)*e=(char*)s;
		return neg?-__builtin_infl():__builtin_infl();
	}
	if(((s[0]|32)=='n')&&((s[1]|32)=='a')&&((s[2]|32)=='n')){
		s+=3;
		if(*s=='('){ const char*q=s+1; while(*q&&*q!=')')q++; if(*q==')')s=q+1; }
		if(e)*e=(char*)s;
		return __builtin_nanl("");
	}
	char *end; int any, nz; long double v=n9_pflt(s,&end,&any,&nz);
	if(!any){ if(e)*e=(char*)nptr; return neg?-0.0L:0.0L; }
	if(e)*e=end;
	if(__builtin_isinf(v)) n9_errno_v = 34;            /* overflow -> ERANGE */
	else if(nz && v==0.0L) n9_errno_v = 34;            /* underflow -> ERANGE */
	return neg?-v:v;
}

unsigned long long strtoull(const char *s, char **e, int b){ int neg; unsigned long long r=n9_ustrto(s,e,b,&neg); return neg?-r:r; }
long long strtoll(const char *s, char **e, int b){ int neg; unsigned long long r=n9_ustrto(s,e,b,&neg); return neg?-(long long)r:(long long)r; }
unsigned long strtoul(const char *s, char **e, int b){ int neg; unsigned long long r=n9_ustrto(s,e,b,&neg); return neg?(unsigned long)-r:(unsigned long)r; }

void *memchr(const void *s, int c, size_t n){ const unsigned char *p=s; for(size_t i=0;i<n;i++) if(p[i]==(unsigned char)c) return (void*)(p+i); return 0; }
/* <inttypes.h> intmax helpers */
long imaxabs(long x){ return x<0?-x:x; }
typedef struct { long quot, rem; } n9_imaxdiv_t;
n9_imaxdiv_t imaxdiv(long a, long b){ n9_imaxdiv_t r; r.quot=a/b; r.rem=a%b; return r; }
/* div/ldiv/lldiv: stdlib integer division returning quot+rem (stdlib.h declares
 * div_t/ldiv_t/lldiv_t as {quot,rem}). */
typedef struct { int quot, rem; } n9_div_t;
typedef struct { long quot, rem; } n9_ldiv_t;
typedef struct { long long quot, rem; } n9_lldiv_t;
n9_div_t   div(int a, int b){ n9_div_t r; r.quot=a/b; r.rem=a%b; return r; }
n9_ldiv_t  ldiv(long a, long b){ n9_ldiv_t r; r.quot=a/b; r.rem=a%b; return r; }
n9_lldiv_t lldiv(long long a, long long b){ n9_lldiv_t r; r.quot=a/b; r.rem=a%b; return r; }
long strtoimax(const char *s, char **e, int b){ return strtoll(s,e,b); }
unsigned long strtoumax(const char *s, char **e, int b){ return strtoull(s,e,b); }
float strtof(const char *s, char **e){
	double d = strtod(s, e); float f = (float)d;
	/* float-range overflow: strtod leaves errno clear (the value fits a double),
	 * but it overflows float — std::stof gates out_of_range on ERANGE. */
	if(__builtin_isinf(f) && !__builtin_isinf(d)) n9_errno_v = 34 /*ERANGE*/;
	return f;
}
/* utoa_ used by strerror above; the printf family (incl. float conv) lives in
 * cc9/runtime/printf.c now. */
static char *utoa_(unsigned long long v, char *p, int base){ char t[32]; int i=0; if(!v)t[i++]='0'; while(v){int d=v%base; t[i++]="0123456789abcdef"[d]; v/=base;} while(i)*p++=t[--i]; return p; }

/* libm (sqrt/sin/exp/atan2/... + f/l variants) now comes from libcc9m.a
 * (openlibm) — a real correctly-rounded libm with full inf/nan/signbit
 * semantics and 80-bit long double. Only integer abs stays here (openlibm is
 * float-only); the fenv functions openlibm references come from runtime/fenv.c. */
int abs(int x){ return x<0?-x:x; }
long labs(long x){ return x<0?-x:x; }
long long llabs(long long x){ return x<0?-x:x; }

/* nan(): libc++ <cmath> re-exports ::nan; openlibm's s_nan didn't link, so map
 * to the compiler builtins (quiet NaN; the tag string is ignored). */
double nan(const char *t){ (void)t; return __builtin_nan(""); }
float  nanf(const char *t){ (void)t; return __builtin_nanf(""); }
long double nanl(const char *t){ (void)t; return __builtin_nanl(""); }

/* tgamma(double): openlibm ships only the float version. Exact for small
 * positive-integer arguments (factorials), Lanczos (g=7) elsewhere with the
 * reflection formula for x < 0.5. tgammaf/tgammal come from openlibm. */
extern double sin(double), sqrt(double), pow(double,double), exp(double), floor(double);
double tgamma(double x){
	if(x != x) return x;                              /* NaN */
	if(__builtin_isinf(x)) return x > 0 ? x : __builtin_nan("");
	if(x == 0.0) return 1.0/x;                        /* +-0 -> +-inf */
	if(x < 0 && x == floor(x)) return __builtin_nan(""); /* negative integer pole */
	if(x > 0 && x == floor(x) && x <= 171.0){         /* exact factorial */
		double r = 1.0; for(double k = 2.0; k < x; k += 1.0) r *= k; return r;
	}
	const double PI = 3.14159265358979323846;
	if(x < 0.5) return PI / (sin(PI*x) * tgamma(1.0 - x));   /* reflection */
	static const double g = 7.0;
	static const double c[9] = {
		0.99999999999980993, 676.5203681218851, -1259.1392167224028,
		771.32342877765313, -176.61502916214059, 12.507343278686905,
		-0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7 };
	x -= 1.0;
	double a = c[0], t = x + g + 0.5;
	for(int i = 1; i < 9; i++) a += c[i] / (x + i);
	return sqrt(2.0*PI) * pow(t, x + 0.5) * exp(-t) * a;
}

/* Wall-clock time via Plan 9's /dev/bintime: reading it yields 8 bytes of
 * big-endian nanoseconds since the Unix epoch. Backs gettimeofday/clock_gettime
 * (what libc++'s <chrono> system/steady clocks call). */
extern long n9_open(const char*, int);
extern long n9_pread(int, void*, long, long long);
extern long n9_close(int);
/* The fd is CACHED. open+read+close per call looks harmless until something
 * asks the time in a hot loop — DOSBox calls SDL_GetTicks() every emulated
 * tick, and three syscalls plus a namespace walk each time made it spend more
 * wall-clock in open(2) than in the emulator (DOOM's startup: minutes).
 *
 * pread at an EXPLICIT offset 0 is what makes a cached fd work: the old code
 * passed -1 ("use the file offset"), which on a reused fd would walk the
 * offset forward and read EOF/zeroes from the second call on. */
static int n9_bintime_fd = -1;
/* Install a cached fd. Threads share the fd table (rfork without RFFDG), so
 * two can open concurrently; the loser closes its own rather than leak it. */
static int n9_bintime_open(void){
	int fd = (int)n9_open("/dev/bintime", 0 /*OREAD*/);
	if(fd < 0) return -1;
	if(__sync_val_compare_and_swap(&n9_bintime_fd, -1, fd) != -1){
		n9_close(fd);
		return n9_bintime_fd;
	}
	return fd;
}
static unsigned long long n9_nsec(void){
	unsigned char b[8];
	int fd = n9_bintime_fd;

	if(fd < 0 && (fd = n9_bintime_open()) < 0)
		return 0;
	if(n9_pread(fd, b, 8, 0) < 8){
		/* fd went stale (a program that closes every fd, or a namespace
		 * change). Drop it and retry once. */
		__sync_val_compare_and_swap(&n9_bintime_fd, fd, -1);
		if((fd = n9_bintime_open()) < 0)
			return 0;
		if(n9_pread(fd, b, 8, 0) < 8)
			return 0;
	}
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
int clock_getres(int clk, void *tsp){
	(void)clk;
	struct n9_timespec *ts = tsp;
	if(ts){ ts->tv_sec = 0; ts->tv_nsec = 1; }   /* /dev/bintime is ns-grained */
	return 0;
}
long time(long *t){ long s = (long)(n9_nsec()/1000000000ULL); if(t)*t=s; return s; }

/* clock(): process CPU time — the Plan 9 way is the Tos cycle counter (captured
 * in crt0), NOT the wall clock. pcycles/cyclefreq is CPU seconds; scale to
 * CLOCKS_PER_SEC (1e6). The divide is split so pcycles*1e6 can't overflow 64 bits.
 * No cycle clock (cyclefreq==0) -> POSIX (clock_t)-1. */
extern long long cc9_tos_pcycles(void);
extern unsigned long long cc9_tos_cyclefreq(void);
long clock(void){
	unsigned long long f = cc9_tos_cyclefreq();
	if(f == 0) return -1;
	unsigned long long c = (unsigned long long)cc9_tos_pcycles();
	return (long)((c / f) * 1000000ULL + ((c % f) * 1000000ULL) / f);
}

/* <fenv.h> (rounding mode + IEEE exception-flag inspection) is implemented for
 * real in runtime/fenv.c — the no-op stubs that used to shadow it lived here and
 * were deleted so fenv.o gets linked. */

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

/* strings.h */
static int cc9_lower(int c){ return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int strcasecmp(const char *a, const char *b){
	while(*a && cc9_lower((unsigned char)*a) == cc9_lower((unsigned char)*b)){ a++; b++; }
	return cc9_lower((unsigned char)*a) - cc9_lower((unsigned char)*b);
}
int strncasecmp(const char *a, const char *b, unsigned long n){
	for(; n; n--, a++, b++){
		int d = cc9_lower((unsigned char)*a) - cc9_lower((unsigned char)*b);
		if(d || !*a) return d;
	}
	return 0;
}

/* stub iconv (see include/iconv.h) */
void *iconv_open(const char *to, const char *from){ (void)to; (void)from; return (void *)-1; }
unsigned long iconv(void *cd, char **in, unsigned long *inb, char **out, unsigned long *outb){
	(void)cd; (void)in; (void)inb; (void)out; (void)outb; return (unsigned long)-1;
}
int iconv_close(void *cd){ (void)cd; return 0; }

char *strtok_r(char *s, const char *sep, char **save){
	if(!s) s = *save;
	while(*s && strchr(sep, *s)) s++;
	if(!*s){ *save = s; return 0; }
	char *tok = s;
	while(*s && !strchr(sep, *s)) s++;
	if(*s){ *s = 0; s++; }
	*save = s;
	return tok;
}
