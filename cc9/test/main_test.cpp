// Bulletproofing: a normal int main() with a global object — crt0 must run its
// constructor before main (the landmine the bare-_start path missed).
extern "C" long n9_pwrite(int, const void *, long, long long);
struct G { int v; G() { v = 99; } };
static G g;
int main()
{
	const char *m = (g.v == 99) ? "global ctor RAN; int main() works\n"
	                            : "global ctor FAILED (g.v != 99)\n";
	long n = 0; while (m[n]) n++;
	n9_pwrite(1, m, n, -1);
	return 0;
}
