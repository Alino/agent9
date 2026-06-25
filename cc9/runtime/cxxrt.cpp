// cc9 minimal C++ runtime for freestanding 9front (the runtime bridge). The
// bits clang/libc++ reference for classes/vtables/allocation without exceptions
// or rtti. Grows as we support more of the language.
extern "C" void n9_exits(const char *);
extern "C" void *malloc(unsigned long);
extern "C" void free(void *);
extern "C" void *aligned_alloc(unsigned long, unsigned long);

#include <new>   // std::align_val_t, nothrow_t, new_handler, bad_alloc decls

// Called if a pure-virtual function is ever invoked (a bug); abort the program.
extern "C" void __cxa_pure_virtual()
{
	n9_exits("cc9: pure virtual call\n");
}

// (__abort_message, __cxa_bad_typeid, __cxa_bad_cast now come from the real
// libcxxabi runtime — exceptions are enabled.)

// Thread-safe-static init guards (Itanium ABI). cc9 is single-threaded, so the
// guard's first byte is just an "initialized" flag — the classic minimal impl.
extern "C" int __cxa_guard_acquire(unsigned long long *g) { return !*(char *)g; }
extern "C" void __cxa_guard_release(unsigned long long *g) { *(char *)g = 1; }
extern "C" void __cxa_guard_abort(unsigned long long *g) { (void)g; }

// operator new/delete over the C allocator. delete MUST call free — a no-op
// leaks every realloc until brk runs out (found via the test suite). free()
// transparently handles both plain and aligned_alloc'd blocks (see n9libc).
//
// WEAK: the global allocation operators are *replaceable* (C++ [basic.stc]) —
// a program (or libc++ test) that defines its own must win, else the link fails
// with a duplicate symbol. Weak makes ours the default only.
#define CC9_WEAK __attribute__((weak))

CC9_WEAK void *operator new(unsigned long n) { void *p = malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
CC9_WEAK void *operator new[](unsigned long n) { void *p = malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
// (placement new/delete come from <new>, inline — don't redefine.)
CC9_WEAK void operator delete(void *p) noexcept { free(p); }
CC9_WEAK void operator delete(void *p, unsigned long) noexcept { free(p); }
CC9_WEAK void operator delete[](void *p) noexcept { free(p); }
CC9_WEAK void operator delete[](void *p, unsigned long) noexcept { free(p); }

// nothrow variants — same allocator (malloc returns 0 on failure, which is the
// nothrow contract).
CC9_WEAK void *operator new(unsigned long n, const std::nothrow_t &) noexcept { return malloc(n); }
CC9_WEAK void *operator new[](unsigned long n, const std::nothrow_t &) noexcept { return malloc(n); }
CC9_WEAK void operator delete(void *p, const std::nothrow_t &) noexcept { free(p); }
CC9_WEAK void operator delete[](void *p, const std::nothrow_t &) noexcept { free(p); }

// over-aligned (C++17) — honor the requested alignment via aligned_alloc (NOT
// plain malloc, which only guarantees 16 bytes). free() recovers the base.
CC9_WEAK void *operator new(unsigned long n, std::align_val_t a) { return aligned_alloc((unsigned long)a, n); }
CC9_WEAK void *operator new[](unsigned long n, std::align_val_t a) { return aligned_alloc((unsigned long)a, n); }
CC9_WEAK void operator delete(void *p, std::align_val_t) noexcept { free(p); }
CC9_WEAK void operator delete[](void *p, std::align_val_t) noexcept { free(p); }
CC9_WEAK void operator delete(void *p, unsigned long, std::align_val_t) noexcept { free(p); }
CC9_WEAK void operator delete[](void *p, unsigned long, std::align_val_t) noexcept { free(p); }
CC9_WEAK void *operator new(unsigned long n, std::align_val_t a, const std::nothrow_t &) noexcept { return aligned_alloc((unsigned long)a, n); }
CC9_WEAK void *operator new[](unsigned long n, std::align_val_t a, const std::nothrow_t &) noexcept { return aligned_alloc((unsigned long)a, n); }
CC9_WEAK void operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept { free(p); }
CC9_WEAK void operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept { free(p); }

// The std::nothrow object (set/get_new_handler now come from libcxxabi).
namespace std { const nothrow_t nothrow{}; }

// std::uncaught_exceptions (was in libc++ exception.cpp, which libcxxabi
// replaces) + a __cxa_demangle stub (only used to prettify terminate messages).
extern "C" unsigned __cxa_uncaught_exceptions() noexcept;
namespace std {
int uncaught_exceptions() noexcept { return (int)__cxa_uncaught_exceptions(); }
bool uncaught_exception() noexcept { return uncaught_exceptions() > 0; }
}
extern "C" char *__cxa_demangle(const char *, char *, unsigned long *, int *status) { if (status) *status = -2; return nullptr; }

// libc++ hardening / error paths.
namespace std { inline namespace __1 {
__attribute__((noreturn)) void __libcpp_verbose_abort(const char *, ...) noexcept {
	n9_exits("cc9: libcxx abort\n"); __builtin_unreachable();
}
}}

// std::__throw_bad_alloc() — allocation-failure path (in std, NOT __1). Under
// -fno-exceptions it aborts. Provided here since we don't link new.cpp.
namespace std {
__attribute__((noreturn)) void __throw_bad_alloc() { throw bad_alloc(); }
}
