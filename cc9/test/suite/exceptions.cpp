// EXPECT: unwind=3 base=ok rethrow=ok custom=7 what=too big PASS
#include "check.h"
#include <stdexcept>
#include <string>

// RAII guard that counts destructors run during stack unwinding.
static int unwound = 0;
struct Guard { ~Guard() { unwound++; } };

__attribute__((noinline)) static void deep() {
	Guard g;                       // must be destroyed as the throw unwinds
	throw std::runtime_error("too big");
}
__attribute__((noinline)) static void mid() {
	Guard g;                       // another frame's local, also unwound
	deep();
}
__attribute__((noinline)) static void top() {
	Guard g;
	mid();
}

struct MyErr {
	int code;
};

int main() {
	std::string what;
	bool base_ok = false, rethrow_ok = false;
	int custom = -1;

	// 1) throw across 3 frames, catch by base reference, RAII dtors run.
	try {
		top();
	} catch (const std::exception &e) {   // catch runtime_error by base
		base_ok = true;
		what = e.what();
	}

	// 2) nested try with rethrow, caught by outer handler.
	try {
		try {
			throw std::logic_error("inner");
		} catch (const std::exception &) {
			throw;                         // rethrow current exception
		}
	} catch (const std::logic_error &) {
		rethrow_ok = true;
	}

	// 3) custom (non-std) exception type, thrown by value.
	try {
		throw MyErr{7};
	} catch (const MyErr &m) {
		custom = m.code;
	}

	emit("unwind=" + std::to_string(unwound) +
	     " base=" + (base_ok ? "ok" : "no") +
	     " rethrow=" + (rethrow_ok ? "ok" : "no") +
	     " custom=" + std::to_string(custom) +
	     " what=" + what +
	     ((unwound == 3 && base_ok && rethrow_ok && custom == 7 && what == "too big")
	          ? " PASS" : " FAIL"));
	return 0;
}
