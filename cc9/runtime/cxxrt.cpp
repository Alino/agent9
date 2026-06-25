// cc9 minimal C++ runtime for freestanding 9front (the start of the runtime
// bridge). Just the bits clang references for classes/vtables without STL,
// exceptions, rtti, or heap. Grows as we support more of the language.
extern "C" void n9_exits(const char *);

// Called if a pure-virtual function is ever invoked (a bug); abort the program.
extern "C" void __cxa_pure_virtual()
{
	n9_exits("cc9: pure virtual call\n");
}

// Heap: operator new/delete over the C allocator. delete MUST call free —
// a no-op leaks every realloc until brk runs out (found via the test suite).
extern "C" void *malloc(unsigned long);
extern "C" void free(void *);
void *operator new(unsigned long n) { return malloc(n); }
void *operator new[](unsigned long n) { return malloc(n); }
void *operator new(unsigned long n, void *p) noexcept { (void)n; return p; }
void operator delete(void *p) noexcept { free(p); }
void operator delete(void *p, unsigned long) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete[](void *p, unsigned long) noexcept { free(p); }

// libc++ hardening / error path.
namespace std { inline namespace __1 {
__attribute__((noreturn)) void __libcpp_verbose_abort(const char *, ...) {
	n9_exits("cc9: libcxx abort\n"); __builtin_unreachable();
}
}}

// C++17 over-aligned new/delete. malloc returns 16-byte-aligned memory, enough
// for the alignments std::vector<scalar>/std::string request.
namespace std { enum class align_val_t : unsigned long {}; }
void *operator new(unsigned long n, std::align_val_t) { return malloc(n); }
void *operator new[](unsigned long n, std::align_val_t) { return malloc(n); }
void operator delete(void *p, std::align_val_t) noexcept { free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { free(p); }
void operator delete(void *p, unsigned long, std::align_val_t) noexcept { free(p); }
void operator delete[](void *p, unsigned long, std::align_val_t) noexcept { free(p); }
