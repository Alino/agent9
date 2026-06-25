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
// WEAK: the global allocation operators are *replaceable* (C++ [basic.stc]) —
// a program (or libc++ test) that defines its own operator new must win, else
// the link fails with a duplicate symbol. Weak makes ours the default only.
#define CC9_WEAK __attribute__((weak))
extern "C" void *malloc(unsigned long);
extern "C" void free(void *);
CC9_WEAK void *operator new(unsigned long n) { return malloc(n); }
CC9_WEAK void *operator new[](unsigned long n) { return malloc(n); }
void *operator new(unsigned long n, void *p) noexcept { (void)n; return p; }
CC9_WEAK void operator delete(void *p) noexcept { free(p); }
CC9_WEAK void operator delete(void *p, unsigned long) noexcept { free(p); }
CC9_WEAK void operator delete[](void *p) noexcept { free(p); }
CC9_WEAK void operator delete[](void *p, unsigned long) noexcept { free(p); }

// libc++ hardening / error path.
namespace std { inline namespace __1 {
__attribute__((noreturn)) void __libcpp_verbose_abort(const char *, ...) {
	n9_exits("cc9: libcxx abort\n"); __builtin_unreachable();
}
}}

// std::__throw_bad_alloc() — the allocation-failure path. Lives in std (NOT the
// __1 inline namespace); under -fno-exceptions it aborts. libc++abi's new.cpp
// would normally supply this, but we don't link new.cpp (it drags in
// aligned_alloc + the __lcxx_override section); provide it directly instead.
namespace std {
__attribute__((noreturn)) void __throw_bad_alloc() {
	n9_exits("cc9: bad_alloc\n"); __builtin_unreachable();
}
}

// C++17 over-aligned new/delete. malloc returns 16-byte-aligned memory, enough
// for the alignments std::vector<scalar>/std::string request.
namespace std { enum class align_val_t : unsigned long {}; }
CC9_WEAK void *operator new(unsigned long n, std::align_val_t) { return malloc(n); }
CC9_WEAK void *operator new[](unsigned long n, std::align_val_t) { return malloc(n); }
CC9_WEAK void operator delete(void *p, std::align_val_t) noexcept { free(p); }
CC9_WEAK void operator delete[](void *p, std::align_val_t) noexcept { free(p); }
CC9_WEAK void operator delete(void *p, unsigned long, std::align_val_t) noexcept { free(p); }
CC9_WEAK void operator delete[](void *p, unsigned long, std::align_val_t) noexcept { free(p); }
