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
i128 __divti3(i128 a, i128 b){
	int neg = 0;
	u128 ua = a < 0 ? (neg ^= 1, (u128)(-a)) : (u128)a;
	u128 ub = b < 0 ? (neg ^= 1, (u128)(-b)) : (u128)b;
	u128 q = udivmod(ua, ub, 0);
	return neg ? -(i128)q : (i128)q;
}
i128 __modti3(i128 a, i128 b){
	u128 ua = a < 0 ? (u128)(-a) : (u128)a;
	u128 ub = b < 0 ? (u128)(-b) : (u128)b;
	u128 r; udivmod(ua, ub, &r);
	return a < 0 ? -(i128)r : (i128)r;
}
