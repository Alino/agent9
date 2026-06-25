// EXPECT: g1=1 g2=2 g3=3
#include "check.h"
static int order=0;
struct G { int n; G(){ n=++order; } };
static G g1, g2;
int main(){
	static G g3;
	emit("g1="+std::to_string(g1.n)+" g2="+std::to_string(g2.n)+" g3="+std::to_string(g3.n));
	return 0;
}
