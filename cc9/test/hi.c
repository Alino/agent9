/*
 * Real clang codegen: _start calls C functions that call the Plan 9 syscall
 * thunks; the string lives in .rodata. Prints "hi\n" then exits. Built on the
 * host (clang + ld.lld), converted ELF->a.out, run on 9front.
 */
long n9_pwrite(int fd, const void *buf, long n, long long off);
void n9_exits(const char *msg);

static long mystrlen(const char *s)
{
	long n = 0;
	while (s[n]) n++;
	return n;
}

void _start(void)
{
	const char *msg = "hi from clang on 9front\n";
	n9_pwrite(1, msg, mystrlen(msg), -1);
	n9_exits(0);
}
