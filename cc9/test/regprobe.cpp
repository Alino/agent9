// Confirm which SysV callee-saved registers the Plan 9 SYSCALL path clobbers.
#include <cstdio>
extern "C" void cc9_regprobe(unsigned long *out);
int main() {
	unsigned long r[6] = {0,0,0,0,0,0};
	cc9_regprobe(r);
	const char *names[6] = {"rbx", "rbp", "r12", "r13", "r14", "r15"};
	unsigned long sent[6] = {0x1111111111111111UL, 0x6666666666666666UL,
	                         0x2222222222222222UL, 0x3333333333333333UL,
	                         0x4444444444444444UL, 0x5555555555555555UL};
	for (int i = 0; i < 6; i++)
		printf("%s after=0x%lx %s\n", names[i], r[i],
		       r[i] == sent[i] ? "(preserved)" : "*** CLOBBERED ***");
	return 0;
}
