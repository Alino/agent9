// First real C++ on 9front: a class with a constructor + methods, a template,
// a virtual call (vtable) — no STL, no exceptions, no rtti, no heap. Proves
// clang's C++ codegen + the C++ ABI work on the 9front target.
extern "C" long n9_pwrite(int fd, const void *buf, long n, long long off);
extern "C" void n9_exits(const char *msg);

struct Shape {
	virtual int area() const = 0;
	virtual ~Shape() {}
};
struct Square : Shape {
	int s;
	Square(int side) : s(side) {}
	int area() const override { return s * s; }
};

template <typename T>
static T add(T a, T b) { return a + b; }

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }

static int fmtint(char *b, int v) {
	if (v == 0) { b[0] = '0'; return 1; }
	char t[16]; int i = 0;
	while (v > 0) { t[i++] = '0' + v % 10; v /= 10; }
	int j = 0; while (i > 0) b[j++] = t[--i];
	return j;
}

extern "C" void _start()
{
	Square sq(6);
	Shape *sh = &sq;                 // virtual dispatch
	int v = add<int>(sh->area(), 6); // 36 + 6 = 42
	char out[64];
	const char *pre = "C++ (vtable+template) on 9front: 6*6+6=";
	long n = slen(pre);
	for (long i = 0; i < n; i++) out[i] = pre[i];
	n += fmtint(out + n, v);
	out[n++] = '\n';
	n9_pwrite(1, out, n, -1);
	n9_exits(0);
}
