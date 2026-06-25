// EXPECT: dtors=8
#include "check.h"
#include <vector>
static int dtors=0;
struct R { ~R(){ dtors++; } };
int main(){
	{ R a,b,c; }            // 3
	{ std::vector<R> v(5); } // 5
	emit("dtors="+std::to_string(dtors)); return 0;
}
