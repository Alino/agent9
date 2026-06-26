// EXPECT: sum=4950 tls=ok once=1 staticctor=1 PASS
#include "check.h"
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <string>

static std::atomic<int> sum{0};
static std::atomic<int> tls_ok{1};
static thread_local int tlsv = -1;          // emutls, must be per-thread

static std::once_flag of;
static std::atomic<int> once_count{0};
static void init_once(){ once_count++; }

struct Ctor { Ctor(){ ctor_count++; } static std::atomic<int> ctor_count; };
std::atomic<int> Ctor::ctor_count{0};
static int touch_static(){ static Ctor c; (void)c; return 0; }   // __cxa_guard race

int main(){
	std::vector<std::thread> ts;
	for(int i=0;i<100;i++) ts.emplace_back([i]{
		tlsv = i;                             // this thread's TLS slot
		std::call_once(of, init_once);        // pthread_once: runs exactly once
		touch_static();                       // function-local static: constructed once
		for(volatile int k=0;k<2000;k++){}    // give threads time to interleave
		if(tlsv != i) tls_ok = 0;             // TLS must not be clobbered by peers
		sum += i;
	});
	for(auto &t : ts) t.join();

	// detached threads must reap themselves (no leak / no hang)
	for(int i=0;i<20;i++) std::thread([]{}).detach();

	bool pass = sum==4950 && tls_ok==1 && once_count==1 && Ctor::ctor_count==1;
	emit(std::string("sum=")+std::to_string(sum.load())+
	     " tls="+(tls_ok?"ok":"NO")+
	     " once="+std::to_string(once_count.load())+
	     " staticctor="+std::to_string(Ctor::ctor_count.load())+
	     (pass?" PASS":" FAIL"));
	return 0;
}
