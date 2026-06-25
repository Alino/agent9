// cc9 minimal C++ runtime for freestanding 9front (the start of the runtime
// bridge). Just the bits clang references for classes/vtables without STL,
// exceptions, rtti, or heap. Grows as we support more of the language.
extern "C" void n9_exits(const char *);

// Called if a pure-virtual function is ever invoked (a bug); abort the program.
extern "C" void __cxa_pure_virtual()
{
	n9_exits("cc9: pure virtual call\n");
}

// Deleting destructors reference operator delete even when we never heap-delete.
// No heap yet -> no-op stubs (would call free() once we have an allocator).
void operator delete(void *) noexcept {}
void operator delete(void *, unsigned long) noexcept {}
void operator delete[](void *) noexcept {}
void operator delete[](void *, unsigned long) noexcept {}

// Heap: operator new over the C malloc shim.
extern "C" void *malloc(unsigned long);
void *operator new(unsigned long n) { return malloc(n); }
void *operator new[](unsigned long n) { return malloc(n); }
void *operator new(unsigned long n, void *p) noexcept { (void)n; return p; }

// libc++ hardening / error path.
namespace std { inline namespace __1 {
__attribute__((noreturn)) void __libcpp_verbose_abort(const char *, ...) {
	n9_exits("cc9: libcxx abort\n"); __builtin_unreachable();
}
}}
