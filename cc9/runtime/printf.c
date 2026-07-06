/* cc9 printf core — a fuller vsnprintf than the original integer-only one:
 * flags (-+ #0space), width and precision (incl. *), length modifiers, and the
 * float conversions f/F e/E g/G a/A. The float path is what libc++'s num_put
 * facet needs to format doubles for iostreams (it calls snprintf("%.*g", ...)).
 * Digit extraction uses long double (80-bit) intermediates + openlibm. */
typedef unsigned long size_t;
typedef __builtin_va_list va_list;
extern void *malloc(size_t);
long double floorl(long double), log10l(long double), powl(long double, long double);
int __n9_isnanl(long double x){ return x != x; }

/* Round significant-digit stream g[0..ng-1] (g[0] nonzero, `sticky` = any nonzero
 * past g[ng-1]) to n digits in out[0..n-1], rounding half-to-even like glibc.
 * E is the decimal exponent of g[0]; returns it, bumped if a carry grows a digit. */
static int n9_round(const char *g, int ng, int sticky, int n, char *out, int E){
	if(ng <= n){
		int i; for(i=0;i<ng;i++) out[i]=g[i];
		for(; i<n; i++) out[i]='0';
		return E;
	}
	for(int i=0;i<n;i++) out[i]=g[i];
	int roundup;
	if(g[n] > '5') roundup=1;
	else if(g[n] < '5') roundup=0;
	else {
		int rest=sticky;
		for(int i=n+1;i<ng && !rest;i++) if(g[i]!='0') rest=1;
		roundup = rest ? 1 : ((out[n-1]-'0') & 1);   /* tie -> to even */
	}
	if(roundup){
		int j=n-1;
		for(; j>=0; j--){ if(out[j]<'9'){ out[j]++; break; } out[j]='0'; }
		if(j<0){ for(int k=n-1;k>0;k--)out[k]=out[k-1]; out[0]='1'; E++; }
	}
	return E;
}

/* Extract `n` significant decimal digits of a positive finite x into out[0..n-1]
 * and return the decimal exponent E such that x ~= 0.d0d1.. * 10^(E+1), i.e.
 * out[0] is the 10^E digit. Rounds half-to-even.
 *
 * Exact path: decompose the 80-bit long double into M*2^E2 (M = 64-bit mantissa)
 * and, when 1 <= x < 2^64 (E2 in [-63,0]), extract the integer part directly and
 * the dyadic fraction with overflow-safe integer *10 stepping — so terminating
 * decimals like 1234567890.125 print exactly (no last-digit drift) at any prec.
 * ponytail: |x|<1 (E2<-63) and |x|>=2^64 (E2>0) fall back to the long-double
 * scaler, which can drift in the far tail; upgrade to a bignum only if a test
 * needs exact digits for those magnitudes. */
static int n9_digits(long double x, int n, char *out){
	if(x == 0){ for(int i=0;i<n;i++)out[i]='0'; return 0; }
	unsigned long long M; unsigned short se;
	__builtin_memcpy(&M,&x,8); __builtin_memcpy(&se,(char*)&x+8,2);
	int be = se & 0x7fff;
	if(be){
		int E2 = be - 16383 - 63;            /* value = M * 2^E2 */
		if(E2 >= -63 && E2 <= 0){
			int shift = -E2;                 /* 0..63 */
			unsigned long long I = shift ? (M >> shift) : M;
			unsigned long long F = shift ? (M & ((1ULL<<shift)-1)) : 0ULL;
			char g[48]; int ng=0;
			char ib[24]; int ni=0; unsigned long long t=I;
			while(t){ ib[ni++]=(char)('0'+(int)(t%10)); t/=10; }   /* I>=1 here */
			int E = ni-1;
			for(int i=ni-1;i>=0;i--) g[ng++]=ib[i];
			while(ng < (int)sizeof g - 1 && F){
				/* F = F*10; digit = F>>shift; F &= (2^shift-1)  (overflow-safe) */
				unsigned long long lo=(F & 0xffffffffULL)*10ULL;
				unsigned long long hi=(F>>32)*10ULL + (lo>>32);
				unsigned long long lo32=lo & 0xffffffffULL; int d;
				if(shift>=32){ int s2=shift-32; d=(int)(hi>>s2);
					F = ((hi & ((1ULL<<s2)-1))<<32) | lo32; }
				else { d=(int)((hi<<(32-shift)) | (lo32>>shift));
					F = lo32 & ((1ULL<<shift)-1); }
				g[ng++]=(char)('0'+d);
			}
			return n9_round(g, ng, F!=0, n, out, E);
		}
	}
	/* fallback: long-double scaler (subnormals, |x|<1, |x|>=2^64) */
	int e = (int)floorl(log10l(x));
	long double m = x / powl(10.0L, (long double)e);
	while(m >= 10.0L){ m /= 10.0L; e++; }
	while(m < 1.0L){ m *= 10.0L; e--; }
	int i;
	for(i=0;i<n;i++){ int d=(int)m; if(d>9)d=9; if(d<0)d=0; out[i]=(char)('0'+d); m=(m-d)*10.0L; }
	if(m >= 5.0L){
		int j=n-1;
		for(; j>=0; j--){ if(out[j]<'9'){ out[j]++; break; } out[j]='0'; }
		if(j<0){ for(int k=n-1;k>0;k--)out[k]=out[k-1]; out[0]='1'; e++; }
	}
	return e;
}

struct buf { char *p; size_t n, len; };
static void bput(struct buf *b, char c){ if(b->len+1 < b->n) b->p[b->len]=c; b->len++; }
static void bputs(struct buf *b, const char *s, int n){ for(int i=0;i<n;i++) bput(b, s[i]); }

static char *utoa_p(unsigned long long v, char *p, int base, int upper){
	char t[32]; int i=0; const char *dig = upper?"0123456789ABCDEF":"0123456789abcdef";
	if(!v) t[i++]='0'; while(v){ t[i++]=dig[v%base]; v/=base; } while(i) *p++=t[--i]; return p;
}

/* format a finite |x| (sign handled by caller) per conv with precision prec. */
static void fmt_float(struct buf *b, long double x, int prec, char conv, int alt, int isL, char *sfx_exp){
	int upper = (conv=='F'||conv=='E'||conv=='G');
	char lc = conv|0x20;   /* lowercase conv */
	if(lc=='a'){
		/* hexadecimal float: 0x1.<nibbles>p±E (double), 0xH.<nibbles>p±E (80-bit).
		 * prec<0 -> shortest (strip trailing-zero nibbles); else round/pad to prec.
		 * The exponent has no min-2-digit padding (unlike %e). Uppercase for A/P/X. */
		int up = (conv=='A');
		const char *hx = up? "0123456789ABCDEF" : "0123456789abcdef";
		char xch = up?'X':'x', pch = up?'P':'p';
		int nv[15]; int nn, lead, E;   /* nibble VALUES 0..15 (so we can round) */
		if(isL){
			unsigned long long m; unsigned short se;
			__builtin_memcpy(&m,&x,8); __builtin_memcpy(&se,(char*)&x+8,2);
			int be=se&0x7fff;
			if(be==0 && m==0){ lead=0; E=0; nn=15; for(int i=0;i<15;i++)nv[i]=0; }
			else { lead=(int)((m>>60)&0xf); E=be-16383-3; nn=15;
				for(int i=0;i<15;i++) nv[i]=(int)((m>>(56-4*i))&0xf); }
		} else {
			double dx=(double)x; unsigned long long u; __builtin_memcpy(&u,&dx,8);
			int be=(int)((u>>52)&0x7ff); unsigned long long man=u&0xfffffffffffffULL;
			/* ponytail: subnormals emit the 0x0.<frac>p-1022 form (lead=0), not glibc's
			 * normalized 0x1...p-1074 — same value, round-trips, no test needs the latter. */
			lead=(be==0)?0:1; E=(be==0)?(man?-1022:0):be-1023; nn=13;
			if(be==0 && man==0) E=0;
			for(int i=0;i<13;i++) nv[i]=(int)((man>>(48-4*i))&0xf);
		}
		/* round to `prec` nibbles (half-to-even) when prec truncates the exact value */
		if(prec>=0 && prec<nn){
			int half = nv[prec], rest=0;
			for(int i=prec+1;i<nn;i++) if(nv[i]){ rest=1; break; }
			int roundup = half>8 || (half==8 && (rest || ((prec? nv[prec-1] : lead)&1)));
			nn=prec;
			if(roundup){
				int j=prec-1;
				for(; j>=0; j--){ if(nv[j]<15){ nv[j]++; break; } nv[j]=0; }
				if(j<0){ if(++lead>15){ lead=1; E+=4; } }
			}
		}
		bput(b,'0'); bput(b,xch); bput(b, hx[lead]);
		int n = (prec<0)? nn : prec;
		if(prec<0){ while(n>0 && nv[n-1]==0) n--; }
		if(n>0 || alt) bput(b,'.');
		for(int i=0;i<n;i++) bput(b, hx[i<nn? nv[i] : 0]);
		bput(b,pch); bput(b, E<0?'-':'+'); int ae=E<0?-E:E;
		char eb[8]; char*ep=utoa_p((unsigned)ae,eb,10,0); bputs(b,eb,(int)(ep-eb));
		(void)sfx_exp; return;
	}
	if(prec < 0) prec = 6;
	if(lc=='g' && prec==0) prec = 1;

	if(lc=='f'){
		/* need digits from 10^E down to 10^-prec */
		int total = 0; int E;
		char digs[80];
		/* significant digits: integer part length + prec */
		E = n9_digits(x, 1, digs);      /* get exponent cheaply */
		int sig = E + 1 + prec; if(sig < 1) sig = 1; if(sig > 75) sig = 75;
		E = n9_digits(x, sig, digs);
		int intlen = E + 1; if(intlen < 1) intlen = 0;
		/* integer part */
		if(intlen <= 0) bput(b,'0');
		else bputs(b, digs, intlen<sig?intlen:sig);
		for(int k=sig; k<intlen; k++) bput(b,'0');
		if(prec>0 || alt){
			bput(b,'.');
			int fracstart = intlen;
			for(int k=0;k<prec;k++){
				int idx = fracstart + k;
				if(idx<0 || idx>=sig) bput(b,'0');
				else bput(b, digs[idx]);
			}
		}
		(void)total;
	} else if(lc=='e'){
		char digs[80]; int sig = prec+1; if(sig>75)sig=75; if(sig<1)sig=1;
		int E = n9_digits(x, sig, digs);
		bput(b, digs[0]);
		if(prec>0 || alt){ bput(b,'.'); for(int k=1;k<=prec;k++) bput(b, k<sig?digs[k]:'0'); }
		bput(b, upper?'E':'e');
		bput(b, E<0?'-':'+'); int ae = E<0?-E:E;
		char eb[8]; char *ep = utoa_p((unsigned)ae, eb, 10, 0); int el=(int)(ep-eb);
		if(el<2){ bput(b,'0'); } bputs(b, eb, el);
	} else { /* g/G */
		char digs[80]; int P = prec; if(P<1)P=1; if(P>75)P=75;
		int E = n9_digits(x, P, digs);
		/* strip trailing zeros unless alt */
		int ndig = P;
		if(!alt){ while(ndig>1 && digs[ndig-1]=='0') ndig--; }
		if(E < -4 || E >= P){
			/* %e style with ndig-1 fractional */
			bput(b, digs[0]);
			if(ndig>1 || alt){ bput(b,'.'); for(int k=1;k<ndig;k++) bput(b,digs[k]); }
			bput(b, upper?'E':'e'); bput(b, E<0?'-':'+');
			int ae=E<0?-E:E; char eb[8]; char*ep=utoa_p((unsigned)ae,eb,10,0); int el=(int)(ep-eb);
			if(el<2)bput(b,'0'); bputs(b,eb,el);
		} else {
			/* %f style */
			int intlen = E+1;
			if(intlen<=0){ bput(b,'0'); }
			else { for(int k=0;k<intlen;k++) bput(b, k<ndig?digs[k]:'0'); }
			int fracdigits = ndig - intlen;
			if(fracdigits>0 || alt){
				bput(b,'.');
				if(intlen<0){ for(int k=0;k<-intlen;k++) bput(b,'0'); }
				for(int k=(intlen<0?0:intlen); k<ndig; k++) bput(b,digs[k]);
			}
		}
	}
	(void)sfx_exp;
}

#ifdef CC9_RECURSE_PROBE
/* DEBUG: if vsnprintf is entered with the stack already deep (runaway recursion),
 * walk our own frame chain and dump return addresses to fd 2, then exit. Catches
 * the recursion cycle in-process (no debugger). Requires -fno-omit-frame-pointer. */
extern char __cc9_main_stack[];
extern long n9_pwrite(int, const void *, long, long long);
extern void n9_exits(const char *);
static void cc9_dump_chain(void){
	n9_pwrite(2, "CC9-RECURSE-CHAIN:\n", 19, -1);
	void **fp = (void **)__builtin_frame_address(0);
	for (int i = 0; i < 40 && fp; i++){
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

int vsnprintf(char *out, size_t n, const char *f, va_list ap){
#ifdef CC9_RECURSE_PROBE
	{ char probe; if ((unsigned long)&probe < (unsigned long)__cc9_main_stack + 224UL*1024*1024) cc9_dump_chain(); }
#endif
	struct buf b = { out, n, 0 };
	for(; *f; f++){
		if(*f != '%'){ bput(&b, *f); continue; }
		f++;
		int left=0, plus=0, space=0, alt=0, zero=0;
		for(;; f++){
			if(*f=='-')left=1; else if(*f=='+')plus=1; else if(*f==' ')space=1;
			else if(*f=='#')alt=1; else if(*f=='0')zero=1; else break;
		}
		int width=0; if(*f=='*'){ width=__builtin_va_arg(ap,int); f++; if(width<0){left=1;width=-width;} }
		else while(*f>='0'&&*f<='9'){ width=width*10+(*f-'0'); f++; }
		int prec=-1; if(*f=='.'){ f++; prec=0; if(*f=='*'){ prec=__builtin_va_arg(ap,int); f++; if(prec<0)prec=-1; } else while(*f>='0'&&*f<='9'){ prec=prec*10+(*f-'0'); f++; } }
		int lng=0; while(*f=='l'){lng++;f++;} int isL=0; if(*f=='L'){isL=1;f++;} if(*f=='h'){f++; if(*f=='h')f++;} if(*f=='z'||*f=='j'||*f=='t')f++;

		char tmp[64]; int tlen=0; char sign=0; const char *body=tmp;
		char c=*f;
		if(c=='d'||c=='i'){ long long v= lng? __builtin_va_arg(ap,long long): __builtin_va_arg(ap,int); unsigned long long u; if(v<0){sign='-';u=(unsigned long long)(-v);} else { if(plus)sign='+'; else if(space)sign=' '; u=(unsigned long long)v; } char*e=utoa_p(u,tmp,10,0); tlen=(int)(e-tmp); }
		else if(c=='u'){ unsigned long long v= lng? __builtin_va_arg(ap,unsigned long long): __builtin_va_arg(ap,unsigned int); char*e=utoa_p(v,tmp,10,0); tlen=(int)(e-tmp); }
		else if(c=='o'){ unsigned long long v= lng? __builtin_va_arg(ap,unsigned long long): __builtin_va_arg(ap,unsigned int); char*e=utoa_p(v,tmp,8,0); tlen=(int)(e-tmp); }
		else if(c=='x'||c=='X'){ unsigned long long v= lng? __builtin_va_arg(ap,unsigned long long): __builtin_va_arg(ap,unsigned int); char*e=utoa_p(v,tmp,16,c=='X'); tlen=(int)(e-tmp); }
		else if(c=='p'){ unsigned long long v=(unsigned long long)__builtin_va_arg(ap,void*); tmp[0]='0';tmp[1]='x'; char*e=utoa_p(v,tmp+2,16,0); tlen=(int)(e-tmp); }
		else if(c=='c'){ tmp[0]=(char)__builtin_va_arg(ap,int); tlen=1; }
		else if(c=='s'){ const char*s=__builtin_va_arg(ap,const char*); if(!s)s="(null)"; int l=0; while(s[l]&&(prec<0||l<prec))l++; /* pad */ int pad=width-l; if(!left)while(pad-->0)bput(&b,' '); bputs(&b,s,l); if(left)while(pad-->0)bput(&b,' '); continue; }
		else if(c=='%'){ tmp[0]='%'; tlen=1; }
		else if((c|0x20)=='f'||(c|0x20)=='e'||(c|0x20)=='g'||(c|0x20)=='a'){
			long double x = isL? __builtin_va_arg(ap,long double): (long double)__builtin_va_arg(ap,double);
			struct buf fb; char fbuf[400]; fb.p=fbuf; fb.n=sizeof fbuf; fb.len=0;
			char fs=0; if(x<0||(x==0&&1.0L/x<0)){ fs='-'; x=-x; } else if(plus)fs='+'; else if(space)fs=' ';
			if(__n9_isnanl(x)){ const char*nn=(c<'a')?"NAN":"nan"; bputs(&fb,nn,3); }
			else if(x > 1.0e4900L){ const char*ii=(c<'a')?"INF":"inf"; bputs(&fb,ii,3); }
			else fmt_float(&fb, x, prec, c, alt, isL, 0);
			int l=(int)fb.len; int total=l+(fs?1:0); int pad=width-total;
			if(!left && !zero) while(pad-->0) bput(&b,' ');
			if(fs)bput(&b,fs);
			if(!left && zero) while(pad-->0) bput(&b,'0');
			bputs(&b, fbuf, l);
			if(left) while(pad-->0) bput(&b,' ');
			continue;
		}
		else { bput(&b,'%'); if(c)bput(&b,c); continue; }

		/* integer/char/%/p: precision = minimum digits (C); pad with leading 0s.
		 * The '0' flag is ignored when a precision is given for an integer. */
		int zpad = 0;
		if((c=='d'||c=='i'||c=='u'||c=='o'||c=='x'||c=='X') && prec>=0){
			if(prec==0 && tlen==1 && tmp[0]=='0') tlen=0;   /* %.0d of 0 -> empty */
			if(tlen < prec) zpad = prec - tlen;
			zero = 0;
		}
		int total = tlen + zpad + (sign?1:0); int pad = width-total;
		if(!left && !zero) while(pad-->0) bput(&b,' ');
		if(sign) bput(&b, sign);
		if(!left && zero) while(pad-->0) bput(&b,'0');
		while(zpad-->0) bput(&b,'0');
		bputs(&b, body, tlen);
		if(left) while(pad-->0) bput(&b,' ');
	}
	if(b.n) b.p[b.len < b.n ? b.len : b.n-1] = 0;
	return (int)b.len;
}
int snprintf(char *out, size_t n, const char *f, ...){ va_list ap; __builtin_va_start(ap,f); int r=vsnprintf(out,n,f,ap); __builtin_va_end(ap); return r; }
/* unbounded variants: callers guarantee the buffer (C89 surface) */
int vsprintf(char *out, const char *f, va_list ap){ return vsnprintf(out, (size_t)1<<30, f, ap); }
int sprintf(char *out, const char *f, ...){ va_list ap; __builtin_va_start(ap,f); int r=vsnprintf(out,(size_t)1<<30,f,ap); __builtin_va_end(ap); return r; }
int vasprintf(char **out, const char *f, va_list ap){
	char buf[256];
	va_list ap2; __builtin_va_copy(ap2, ap);
	int n = vsnprintf(buf, sizeof buf, f, ap);
	if(n < 0){ __builtin_va_end(ap2); *out=0; return -1; }
	char *s = malloc((size_t)n + 1);
	if(!s){ __builtin_va_end(ap2); *out=0; return -1; }
	if(n < (int)sizeof buf){ for(int i=0;i<=n;i++) s[i]=buf[i]; }       /* fit: copy */
	else vsnprintf(s, (size_t)n + 1, f, ap2);                          /* reformat exact */
	__builtin_va_end(ap2);
	*out=s; return n;
}
int asprintf(char **out, const char *f, ...){ va_list ap; __builtin_va_start(ap,f); int r=vasprintf(out,f,ap); __builtin_va_end(ap); return r; }

/* sscanf — %d/i/u/x/o (+l/ll), %f/e/g (+l), %s, %c, %%, width, * suppression. */
extern int isspace(int);
extern long long strtoll(const char*,char**,int); extern unsigned long long strtoull(const char*,char**,int);
extern double strtod(const char*,char**);
extern long double strtold(const char*,char**);
int vsscanf(const char *s, const char *f, va_list ap){
	int count=0;
	for(; *f; f++){
		if(isspace((unsigned char)*f)){ while(isspace((unsigned char)*s))s++; continue; }
		if(*f != '%'){ if(*s != *f) break; s++; continue; }
		f++;
		int suppress=0; if(*f=='*'){ suppress=1; f++; }
		int width=0; while(*f>='0'&&*f<='9'){ width=width*10+(*f-'0'); f++; }
		int lng=0; while(*f=='l'){lng++;f++;} if(*f=='h')f++; int isL=0; if(*f=='L'){isL=1;f++;} if(*f=='z'||*f=='j')f++;
		char c=*f;
		if(c!='c' && c!='%') while(isspace((unsigned char)*s)) s++;
		if(c=='d'||c=='i'||c=='u'||c=='x'||c=='X'||c=='o'){
			char *end; int base=(c=='x'||c=='X')?16:(c=='o')?8:(c=='i')?0:10;
			int uns=(c=='u'||c=='x'||c=='X'||c=='o');
			unsigned long long v = uns ? strtoull(s,&end,base) : (unsigned long long)strtoll(s,&end,base);
			if(end==s) break;
			if(!suppress){ void*p=__builtin_va_arg(ap,void*); if(lng>=2)*(long long*)p=(long long)v; else if(lng==1)*(long*)p=(long)v; else *(int*)p=(int)v; }
			s=end; count++;
		} else if(c=='f'||c=='e'||c=='g'||c=='F'||c=='E'||c=='G'||c=='a'){
			char *end;
			if(isL){ long double v=strtold(s,&end); if(end==s)break; if(!suppress)*(long double*)__builtin_va_arg(ap,void*)=v; }
			else { double v=strtod(s,&end); if(end==s)break; if(!suppress){ void*p=__builtin_va_arg(ap,void*); if(lng)*(double*)p=v; else *(float*)p=(float)v; } }
			s=end; count++;
		} else if(c=='s'){
			char *out=suppress?0:__builtin_va_arg(ap,char*); int n=0;
			while(*s && !isspace((unsigned char)*s) && (width==0||n<width)){ if(out)out[n]=*s; n++; s++; }
			if(out)out[n]=0; if(n)count++; else break;
		} else if(c=='c'){
			char *out=suppress?0:__builtin_va_arg(ap,char*); int w=width?width:1;
			int got=0; for(int i=0;i<w&&*s;i++){ if(out)out[i]=*s; s++; got=1; } if(got)count++;
		} else if(c=='%'){ if(*s=='%')s++; else break; }
		else break;
	}
	return count;
}
int sscanf(const char *s, const char *f, ...){ va_list ap; __builtin_va_start(ap,f); int r=vsscanf(s,f,ap); __builtin_va_end(ap); return r; }
