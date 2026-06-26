// bfjit — a brainfuck JIT for 9front. Compiles BF to x86-64 machine code at
// RUNTIME and executes it. This is impossible on a stock 9front (NX-enforced);
// it works because cc9's optional W^X kernel patch lets a process request an
// executable segment (segattach with SG_EXEC, gated by plan9.ini wxallow=1).
//
//   build:  cc9 build cc9/test/bfjit.cpp
//   run:    needs the patched kernel + wxallow=1  (else the call faults: NX)
#include <cstdio>
#include <cstdint>
#include <vector>
#include <initializer_list>

extern "C" void *n9_segattach(unsigned long attr, const char *cls, void *va, unsigned long len);
enum { SG_EXEC = 0x800 };

int main() {
	// classic "Hello World!" brainfuck
	const char *bf =
	    "++++++++[>++++[>++>+++>+++>+<<<<-]>+>+>->>+[<]<-]>>.>---.+++++++..+++."
	    ">>.<-.<.+++.------.--------.>>+.>++.";

	std::vector<uint8_t> code;
	auto emit  = [&](std::initializer_list<uint8_t> b){ for (uint8_t x : b) code.push_back(x); };
	auto emit32 = [&](int32_t v){ for (int i = 0; i < 4; i++) code.push_back((uint8_t)(v >> (8*i))); };
	auto patch32 = [&](size_t at, int32_t v){ for (int i = 0; i < 4; i++) code[at+i] = (uint8_t)(v >> (8*i)); };

	// prologue: function (void)(uint8_t *tape /*rdi*/, char *out /*rsi*/)
	emit({0x53, 0x41,0x55, 0x48,0x89,0xfb, 0x49,0x89,0xf5}); // push rbx; push r13; mov rbx,rdi; mov r13,rsi

	std::vector<size_t> loops;
	for (const char *p = bf; *p; p++) switch (*p) {
		case '>': emit({0x48,0xff,0xc3}); break;                              // inc rbx
		case '<': emit({0x48,0xff,0xcb}); break;                              // dec rbx
		case '+': emit({0xfe,0x03});      break;                              // inc byte [rbx]
		case '-': emit({0xfe,0x0b});      break;                              // dec byte [rbx]
		case '.': emit({0x8a,0x03, 0x41,0x88,0x45,0x00, 0x49,0xff,0xc5}); break; // mov al,[rbx]; mov [r13],al; inc r13
		case '[': emit({0x80,0x3b,0x00, 0x0f,0x84}); emit32(0); loops.push_back(code.size()); break; // cmp byte[rbx],0; je <patch>
		case ']': {
			size_t open = loops.back(); loops.pop_back();
			emit({0xe9}); size_t jrel = code.size(); emit32(0);              // jmp back to the matching cmp
			patch32(jrel, (int32_t)((open-9) - (jrel+4)));                   // ']' -> re-test at the '[' cmp
			patch32(open-4, (int32_t)(code.size() - open));                  // '[' je -> loop exit (here)
			break;
		}
	}
	emit({0x41,0x5d, 0x5b, 0xc3});  // epilogue: pop r13; pop rbx; ret

	void *mem = n9_segattach(SG_EXEC, "memory", 0, (code.size() + 0xfff) & ~0xffful);
	if (mem == (void *)-1 || mem == nullptr) {
		printf("segattach(SG_EXEC) failed (need the W^X patch + plan9.ini wxallow=1)\n");
		return 1;
	}
	for (size_t i = 0; i < code.size(); i++) ((uint8_t *)mem)[i] = code[i];

	unsigned char tape[30000] = {0};
	char out[256] = {0};
	auto fn = (void (*)(unsigned char *, char *))mem;
	fn(tape, out);   // <-- executes runtime-generated machine code

	printf("bfjit: compiled %d BF ops to %d bytes of x86-64, executed it ->\n  \"%s\"\n",
	       0, (int)code.size(), out);
	return 0;
}
