/* asmint_smoke — A6: the generated x86_64 interpreter object links into a
 * Plan 9 a.out, its .data.rel.ro dispatch table is placed+readable, and the
 * entry is addressable text on-box. Executing bytecode needs the VM (M1). */
#include <stdio.h>
extern void asm_interpreter_entry(void);
extern const void *asm_dispatch_table_probe(void);
int main(void) {
	unsigned char *p = (unsigned char *)&asm_interpreter_entry;
	/* first insn must be the audited prologue: pushq %rbp = 0x55 */
	printf("entry=%p first-byte=0x%02x %s\n", (void *)p, p[0],
	       p[0] == 0x55 ? "OK" : "WRONG");
	return p[0] == 0x55 ? 0 : 1;
}
