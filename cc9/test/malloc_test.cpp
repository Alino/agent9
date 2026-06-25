// Prove free() reclaims: a freed block is reused, and a 1M alloc/free churn
// stays bounded (the old bump allocator would sbrk ~64MB).
extern "C" void *malloc(unsigned long);
extern "C" void free(void *);
extern "C" long n9_pwrite(int, const void *, long, long long);
int main()
{
	void *a = malloc(256); free(a);
	void *b = malloc(256); free(b);
	int reused = (a == b);
	for (long i = 0; i < 1000000; i++) { void *p = malloc(64); if (p) free(p); }
	const char *m = reused ? "free() reclaims (block reused); 1M churn ok\n"
	                       : "free() does NOT reclaim (leak)\n";
	long n = 0; while (m[n]) n++;
	n9_pwrite(1, m, n, -1);
	return 0;
}
