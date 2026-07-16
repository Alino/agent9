// cc9 minimal C++ runtime for freestanding 9front (the runtime bridge). The
// bits clang/libc++ reference for classes/vtables/allocation without exceptions
// or rtti. Grows as we support more of the language.
extern "C" void n9_exits(const char *);
extern "C" void *malloc(unsigned long);
extern "C" void free(void *);
extern "C" void *aligned_alloc(unsigned long, unsigned long);

#include <new>   // std::align_val_t, nothrow_t, new_handler, bad_alloc decls
#include <exception>   // std::terminate

// Runs a worker thread's start function. [thread.thread.constr]/5: if it exits
// via an exception, std::terminate is called. cc9's DWARF unwinder can't unwind
// off the top of a worker stack (the C trampoline in pthread.c has no landing
// pad), so the exception would otherwise hang the thread; catch it here so the
// installed terminate handler runs. Called from pthread.c's trampoline.
extern "C" void *cc9_thread_invoke(void *(*start)(void *), void *arg)
{
	try { return start(arg); }
	catch (...) { std::terminate(); }
}

// Called if a pure-virtual function is ever invoked (a bug); abort the program.
extern "C" void __cxa_pure_virtual()
{
	n9_exits("cc9: pure virtual call\n");
}

// Called if a deleted virtual function is ever invoked (a bug); abort.
extern "C" void __cxa_deleted_virtual()
{
	n9_exits("cc9: deleted virtual call\n");
}

// (__abort_message, __cxa_bad_typeid, __cxa_bad_cast now come from the real
// libcxxabi runtime — exceptions are enabled.)

// Thread-safe-static init guards (Itanium ABI). cc9 now runs real threads (and
// libc++ is built with threading), so two threads first-touching the same
// function-local static must NOT both construct. byte[0] = initialized (the flag
// the compiler's fast path reads); byte[1] = in-progress. A global lock makes
// the check-and-claim atomic; the constructor runs WITHOUT the lock (so nested
// static init can't deadlock) while other threads spin-yield until byte[0] is set.
extern "C" int n9_semacquire(int *, int);
extern "C" int n9_semrelease(int *, int);
extern "C" long n9_sleep(long);
static int cc9_guard_lock = 1;
extern "C" int __cxa_guard_acquire(unsigned long long *g) {
	unsigned char *b = (unsigned char *)g;
	// Upstream libc++abi (src/cxa_guard_impl.h) opens with exactly this:
	//     // if guard_byte is non-zero, we have already completed initialization
	//     return guard_byte.load(_AO_Acquire) != UNSET;
	// i.e. ONE atomic load and out — the lock is only for the genuinely
	// uninitialized path, once per static, ever. Without this fast path every
	// function-local static access (libc++/mozjs/LLVM have thousands, and they
	// stay hot forever) took a process-global KERNEL semaphore: ~2.7us per call
	// and every thread convoys on one lock. Release below stores b[0] under the
	// lock, which pairs with this acquire.
	if (__atomic_load_n(&b[0], __ATOMIC_ACQUIRE)) return 0;
	for (;;) {
		n9_semacquire(&cc9_guard_lock, 1);
		if (b[0]) { n9_semrelease(&cc9_guard_lock, 1); return 0; }        // already done
		if (!b[1]) { b[1] = 1; n9_semrelease(&cc9_guard_lock, 1); return 1; } // we construct
		n9_semrelease(&cc9_guard_lock, 1);                               // another thread is constructing
		n9_sleep(0);
	}
}
extern "C" void __cxa_guard_release(unsigned long long *g) {
	unsigned char *b = (unsigned char *)g;
	n9_semacquire(&cc9_guard_lock, 1);
	b[1] = 0;
	__atomic_store_n(&b[0], (unsigned char)1, __ATOMIC_RELEASE);   // pairs w/ the acquire fast path
	n9_semrelease(&cc9_guard_lock, 1);
}
extern "C" void __cxa_guard_abort(unsigned long long *g) {
	unsigned char *b = (unsigned char *)g;
	n9_semacquire(&cc9_guard_lock, 1); b[1] = 0; n9_semrelease(&cc9_guard_lock, 1);
}

// operator new/delete over the C allocator. delete MUST call free — a no-op
// leaks every realloc until brk runs out (found via the test suite). free()
// transparently handles both plain and aligned_alloc'd blocks (see n9libc).
//
// WEAK: the global allocation operators are *replaceable* (C++ [basic.stc]) —
// a program (or libc++ test) that defines its own must win, else the link fails
// with a duplicate symbol. Weak makes ours the default only.
#define CC9_WEAK __attribute__((weak))

// On failure, run the installed new_handler and retry; throw bad_alloc only when
// no handler is set ([new.delete.single]).
#ifdef CC9_RECURSE_PROBE
extern "C" char __cc9_main_stack[];
extern "C" long n9_pwrite(int, const void *, long, long long);
extern "C" void n9_exits(const char *);
// DEBUG: if operator new is hit with the stack already deep (runaway recursion),
// walk our own frame chain and dump return addresses to fd 2, then exit. Catches
// the recursion cycle in-process (operator new is allocated per recursion level).
static void cc9_dump_chain_new() {
	n9_pwrite(2, "CC9-RECURSE-CHAIN:\n", 19, -1);
	void **fp = (void **)__builtin_frame_address(0);
	for (int i = 0; i < 50 && fp; i++) {
		void *ret = fp[1];
		char b[20]; int k = 0; b[k++]='0'; b[k++]='x';
		unsigned long v = (unsigned long)ret;
		for (int j = 15; j >= 0; j--) { int d = (v>>(j*4))&0xf; b[k++] = d<10?'0'+d:'a'+d-10; }
		b[k++]='\n'; n9_pwrite(2, b, k, -1);
		void **nx = (void **)fp[0];
		if (nx <= fp) break;
		fp = nx;
	}
	n9_exits("cc9-recurse");
}
#endif
static void *cc9_new(unsigned long n) {
#ifdef CC9_RECURSE_PROBE
	{ char probe; if ((unsigned long)&probe < (unsigned long)__cc9_main_stack + 236UL*1024*1024) cc9_dump_chain_new(); }
#endif
	for (;;) {
		if (void *p = malloc(n ? n : 1)) return p;
		std::new_handler h = std::get_new_handler();
		if (!h) throw std::bad_alloc();
		h();
	}
}
CC9_WEAK void *operator new(unsigned long n) { return cc9_new(n); }
// Array new is NOT a separate allocator: its default behavior is to call the
// (replaceable) scalar ::operator new — so a TU that replaces only operator new
// still wins for `new T[]`. [new.delete.array] / libcxxabi stdlib_new_delete.cpp.
CC9_WEAK void *operator new[](unsigned long n) { return ::operator new(n); }
// (placement new/delete come from <new>, inline — don't redefine.)
// Only the two CORE forms call free(); ALL other forms (sized, nothrow) delegate
// to the core, per [new.delete]. This keeps operator delete REPLACEABLE: a TU
// that replaces only `operator delete(void*)` (e.g. libc++'s count_new.h leak
// checker) still catches deallocations the compiler routes through sized-delete
// (-fsized-deallocation, the C++14+ default). Calling free() directly here would
// bypass the user's replacement and falsely report leaks.
CC9_WEAK void operator delete(void *p) noexcept { free(p); }
// Array delete forwards to the (replaceable) scalar ::operator delete — NOT free
// directly — so a TU that replaces only operator delete catches `delete[]` too.
CC9_WEAK void operator delete[](void *p) noexcept { ::operator delete(p); }
CC9_WEAK void operator delete(void *p, unsigned long) noexcept { ::operator delete(p); }
CC9_WEAK void operator delete[](void *p, unsigned long) noexcept { ::operator delete[](p); }

// nothrow variants — same allocator (malloc returns 0 on failure, which is the
// nothrow contract).
// nothrow: try the throwing path, return null instead of propagating bad_alloc.
// (also fixes n==0 returning a unique non-null pointer via cc9_new's n?n:1.)
CC9_WEAK void *operator new(unsigned long n, const std::nothrow_t &) noexcept { try { return ::operator new(n); } catch (...) { return nullptr; } }
CC9_WEAK void *operator new[](unsigned long n, const std::nothrow_t &) noexcept { try { return ::operator new[](n); } catch (...) { return nullptr; } }
CC9_WEAK void operator delete(void *p, const std::nothrow_t &) noexcept { ::operator delete(p); }
CC9_WEAK void operator delete[](void *p, const std::nothrow_t &) noexcept { ::operator delete[](p); }

// over-aligned (C++17) — honor the requested alignment via aligned_alloc (NOT
// plain malloc, which only guarantees 16 bytes). free() recovers the base.
static void *cc9_new_aligned(unsigned long n, unsigned long a) {
	for (;;) {
		if (void *p = aligned_alloc(a, n ? n : 1)) return p;
		std::new_handler h = std::get_new_handler();
		if (!h) throw std::bad_alloc();
		h();
	}
}
CC9_WEAK void *operator new(unsigned long n, std::align_val_t a) { return cc9_new_aligned(n, (unsigned long)a); }
CC9_WEAK void *operator new[](unsigned long n, std::align_val_t a) { return ::operator new(n, a); }
// aligned core forms call free(); aligned sized/nothrow delegate to them.
CC9_WEAK void operator delete(void *p, std::align_val_t) noexcept { free(p); }
CC9_WEAK void operator delete[](void *p, std::align_val_t a) noexcept { ::operator delete(p, a); }
CC9_WEAK void operator delete(void *p, unsigned long, std::align_val_t a) noexcept { ::operator delete(p, a); }
CC9_WEAK void operator delete[](void *p, unsigned long, std::align_val_t a) noexcept { ::operator delete[](p, a); }
CC9_WEAK void *operator new(unsigned long n, std::align_val_t a, const std::nothrow_t &) noexcept { try { return ::operator new(n, a); } catch (...) { return nullptr; } }
CC9_WEAK void *operator new[](unsigned long n, std::align_val_t a, const std::nothrow_t &) noexcept { try { return ::operator new[](n, a); } catch (...) { return nullptr; } }
CC9_WEAK void operator delete(void *p, std::align_val_t a, const std::nothrow_t &) noexcept { ::operator delete(p, a); }
CC9_WEAK void operator delete[](void *p, std::align_val_t a, const std::nothrow_t &) noexcept { ::operator delete[](p, a); }

// The std::nothrow object (set/get_new_handler now come from libcxxabi).
namespace std { const nothrow_t nothrow{}; }

// std::uncaught_exceptions (libc++'s exception.cpp isn't linked — it would clash
// with the libcxxabi runtime; exception_ptr comes from runtime/exception_ptr.cpp)
extern "C" unsigned __cxa_uncaught_exceptions() noexcept;
namespace std {
int uncaught_exceptions() noexcept { return (int)__cxa_uncaught_exceptions(); }
bool uncaught_exception() noexcept { return uncaught_exceptions() > 0; }
}
/* __cxa_demangle comes from the real libcxxabi demangler (cxa_demangle.cpp,
 * built in build-runtime.sh) — it makes terminate/backtrace messages readable
 * and passes the demangler conformance suite. (Was a -2 stub.) */

// libc++ hardening / error paths.
namespace std { inline namespace __1 {
// weak: tests (check_assertion.h) replace this with their own interceptor.
__attribute__((noreturn, weak)) void __libcpp_verbose_abort(const char *, ...) noexcept {
	/* Route through abort() so a user-installed SIGABRT handler runs (libc++'s
	 * hardening/verbose-abort contract; the death-test harness replaces this
	 * weak symbol with its own interceptor). */
	::abort(); __builtin_unreachable();
}
}}

// std::__throw_bad_alloc() — allocation-failure path (in std, NOT __1). Under
// -fno-exceptions it aborts. Provided here since we don't link new.cpp.
namespace std {
__attribute__((noreturn)) void __throw_bad_alloc() { throw bad_alloc(); }
}
