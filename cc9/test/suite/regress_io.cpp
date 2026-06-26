// EXPECT: asprintf=2000 file=3000 preserve=ok prec=00042 prec0= hexscan=255 PASS
#include "check.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fcntl.h>
extern "C" int asprintf(char**, const char*, ...);
extern "C" int open(const char*, int, ...);
extern "C" long write(int, const void*, size_t);
extern "C" long read(int, void*, size_t);
extern "C" int close(int);
int main(){
	// asprintf must not truncate at the internal staging-buffer size
	char *out=0; int an = asprintf(&out, "%s", std::string(2000,'y').c_str());
	bool asprintf_ok = an==2000 && out && std::strlen(out)==2000;
	std::free(out);

	// fprintf >1024 must reach the file (vfprintf used to truncate); also exercises
	// the fwrite/fread short-transfer loops on a real file.
	const char *path = "/tmp/cc9_regress_io";
	FILE *f = std::fopen(path, "w");
	std::string big(3000, 'z');
	int wn = f ? std::fprintf(f, "%s", big.c_str()) : -1;
	if(f) std::fclose(f);
	FILE *r = std::fopen(path, "r");
	char rb[4096]; size_t rn = r ? std::fread(rb, 1, sizeof rb, r) : 0;
	if(r) std::fclose(r);
	bool file_ok = wn==3000 && rn==3000;

	// O_CREAT WITHOUT O_TRUNC must NOT wipe an existing file
	int fd = open(path, O_RDWR);          // path already holds 3000 'z'
	char c0=0; bool preserve = fd>=0 && read(fd, &c0, 1)==1 && c0=='z';
	if(fd>=0) close(fd);

	// integer precision + hex scan
	char pb[32]; std::snprintf(pb, sizeof pb, "%.5d", 42);   // 00042
	char p0[32]; std::snprintf(p0, sizeof p0, "%.0d", 0);    // (empty)
	unsigned int hx=0; std::sscanf("ff", "%x", &hx);

	bool pass = asprintf_ok && file_ok && preserve &&
	            std::string(pb)=="00042" && p0[0]==0 && hx==255;
	emit(std::string("asprintf=")+std::to_string(an)+" file="+std::to_string((int)rn)+
	     " preserve="+(preserve?"ok":"NO")+" prec="+pb+" prec0="+p0+
	     " hexscan="+std::to_string(hx)+(pass?" PASS":" FAIL"));
	return 0;
}
