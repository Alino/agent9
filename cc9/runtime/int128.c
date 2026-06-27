/* 128-bit integer division builtins (compiler-rt's __divti3 family), emitted by
 * clang for __int128 division — used by libc++'s <filesystem> file-time math.
 * Bit-by-bit long division using only constant shifts/compares (no __int128
 * division or variable shifts, so it doesn't recurse into other builtins). */
typedef unsigned __int128 u128;
typedef __int128 i128;

static u128 udivmod(u128 a, u128 b, u128 *rem){
	if(b == 0){ if(rem) *rem = 0; return 0; }
	u128 q = 0, r = 0;
	for(int i = 0; i < 128; i++){
		r = (r << 1) | ((a >> 127) & 1);
		a <<= 1;
		if(r >= b){ r -= b; q = (q << 1) | 1; }
		else q <<= 1;
	}
	if(rem) *rem = r;
	return q;
}
u128 __udivti3(u128 a, u128 b){ return udivmod(a, b, 0); }
u128 __umodti3(u128 a, u128 b){ u128 r; udivmod(a, b, &r); return r; }
/* negate in the UNSIGNED domain (-(u128)a), well-defined even for INT128_MIN —
 * (u128)(-a) on signed INT128_MIN would be overflow UB. */
i128 __divti3(i128 a, i128 b){
	int neg = 0;
	u128 ua = a < 0 ? (neg ^= 1, -(u128)a) : (u128)a;
	u128 ub = b < 0 ? (neg ^= 1, -(u128)b) : (u128)b;
	u128 q = udivmod(ua, ub, 0);
	return neg ? -(i128)q : (i128)q;
}
i128 __modti3(i128 a, i128 b){
	u128 ua = a < 0 ? -(u128)a : (u128)a;
	u128 ub = b < 0 ? -(u128)b : (u128)b;
	u128 r; udivmod(ua, ub, &r);
	return a < 0 ? -(i128)r : (i128)r;
}

/* __int128 <-> double conversions (compiler-rt's __floatXtidf / __fixdfti family).
 * clang emits these for code mixing __int128 with double; some libc++ tests pull
 * them in. 64-bit halves convert via native SSE2 (no further libcall). The
 * high*2^64+low form double-rounds, which is fine for representable test values. */
#define TWO64 18446744073709551616.0   /* 2^64 */
double __floatuntidf(u128 a){
	double hi = (double)(unsigned long long)(a >> 64);
	double lo = (double)(unsigned long long)(a & 0xFFFFFFFFFFFFFFFFULL);
	return hi * TWO64 + lo;
}
double __floattidf(i128 a){
	return a < 0 ? -__floatuntidf(-(u128)a) : __floatuntidf((u128)a);
}
u128 __fixunsdfti(double a){
	if(a < 1.0) return 0;                              /* negatives and [0,1) */
	if(a < TWO64) return (u128)(unsigned long long)a;
	unsigned long long hi = (unsigned long long)(a / TWO64);
	double rem = a - (double)hi * TWO64;
	if(rem < 0) rem = 0;
	return ((u128)hi << 64) | (u128)(unsigned long long)rem;
}
i128 __fixdfti(double a){
	return a < 0 ? -(i128)__fixunsdfti(-a) : (i128)__fixunsdfti(a);
}
/* float (sf) variants */
float __floatuntisf(u128 a){
	float hi = (float)(unsigned long long)(a >> 64);
	float lo = (float)(unsigned long long)(a & 0xFFFFFFFFFFFFFFFFULL);
	return hi * (float)TWO64 + lo;
}
float __floattisf(i128 a){ return a < 0 ? -__floatuntisf(-(u128)a) : __floatuntisf((u128)a); }
u128 __fixunssfti(float a){
	if(a < 1.0f) return 0;
	if(a < (float)TWO64) return (u128)(unsigned long long)a;
	unsigned long long hi = (unsigned long long)(a / (float)TWO64);
	float rem = a - (float)hi * (float)TWO64; if(rem < 0) rem = 0;
	return ((u128)hi << 64) | (u128)(unsigned long long)rem;
}
i128 __fixsfti(float a){ return a < 0 ? -(i128)__fixunssfti(-a) : (i128)__fixunssfti(a); }
/* long double (xf, x87 80-bit) variants */
long double __floatuntixf(u128 a){
	long double hi = (long double)(unsigned long long)(a >> 64);
	long double lo = (long double)(unsigned long long)(a & 0xFFFFFFFFFFFFFFFFULL);
	return hi * (long double)TWO64 + lo;
}
long double __floattixf(i128 a){ return a < 0 ? -__floatuntixf(-(u128)a) : __floatuntixf((u128)a); }
u128 __fixunsxfti(long double a){
	if(a < 1.0L) return 0;
	if(a < (long double)TWO64) return (u128)(unsigned long long)a;
	unsigned long long hi = (unsigned long long)(a / (long double)TWO64);
	long double rem = a - (long double)hi * (long double)TWO64; if(rem < 0) rem = 0;
	return ((u128)hi << 64) | (u128)(unsigned long long)rem;
}
i128 __fixxfti(long double a){ return a < 0 ? -(i128)__fixunsxfti(-a) : (i128)__fixunsxfti(a); }
