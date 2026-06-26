// EXPECT: calloc_ovf=ok aligned=ok realloc=ok hex=255 auto=26 oct=511 neg=-42 exp=1500 PASS
#include "check.h"
#include <cstdlib>
#include <cstring>
#include <string>
int main(){
	// calloc overflow must return null, not silently under-allocate
	void *p = std::calloc((size_t)-1, 2);
	bool calloc_ok = (p == nullptr); std::free(p);

	// over-aligned alloc + realloc must preserve data and not corrupt the heap
	unsigned char *a = (unsigned char*)std::aligned_alloc(64, 100);
	bool aligned_ok = a && ((unsigned long)a % 64 == 0);
	if(a) std::memset(a, 0xAB, 100);
	unsigned char *a2 = (unsigned char*)std::realloc(a, 400);
	bool realloc_ok = a2 != nullptr;
	if(a2) for(int i=0;i<100;i++) if(a2[i]!=0xAB){ realloc_ok=false; break; }
	std::free(a2);

	// strto* must honor base / 0x / sign / exponent
	long hex  = std::strtol("0xff", 0, 16);      // 255
	long autob= std::strtol("0x1a", 0, 0);       // 26
	unsigned long oct = std::strtoul("777", 0, 8); // 511
	long neg  = std::strtol("  -42xyz", 0, 10);  // -42 (skip ws, sign)
	double e  = std::strtod("1.5e3", 0);         // 1500

	bool pass = calloc_ok && aligned_ok && realloc_ok &&
	            hex==255 && autob==26 && oct==511 && neg==-42 && e==1500.0;
	emit(std::string("calloc_ovf=")+(calloc_ok?"ok":"NO")+
	     " aligned="+(aligned_ok?"ok":"NO")+" realloc="+(realloc_ok?"ok":"NO")+
	     " hex="+std::to_string(hex)+" auto="+std::to_string(autob)+
	     " oct="+std::to_string(oct)+" neg="+std::to_string(neg)+
	     " exp="+std::to_string((long)e)+(pass?" PASS":" FAIL"));
	return 0;
}
