// The real target: std::vector from libc++ on 9front.
#include <vector>
extern "C" long n9_pwrite(int, const void *, long, long long);
extern "C" void n9_exits(const char *);

static int fmtint(char *b, int v) {
	if (v == 0) { b[0] = '0'; return 1; }
	char t[16]; int i = 0;
	while (v > 0) { t[i++] = '0' + v % 10; v /= 10; }
	int j = 0; while (i > 0) b[j++] = t[--i];
	return j;
}

int main()
{
	std::vector<int> v;
	for (int i = 1; i <= 8; i++) v.push_back(i * i);  // squares
	int sum = 0;
	for (int x : v) sum += x;                          // 1+4+...+64 = 204
	char out[64];
	const char *pre = "std::vector<int> sum of squares 1..8 = ";
	long n = 0; while (pre[n]) { out[n] = pre[n]; n++; }
	n += fmtint(out + n, sum);
	out[n++] = '\n';
	n9_pwrite(1, out, n, -1);
	return 0;
}
