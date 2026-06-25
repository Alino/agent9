// EXPECT: uniq=42 shared=hi use=2
#include "check.h"
#include <memory>
int main(){
	auto p=std::make_unique<int>(42);
	auto q=std::make_shared<std::string>("hi");
	auto r=q;
	emit("uniq="+std::to_string(*p)+" shared="+*q+" use="+std::to_string(q.use_count()));
	return 0;
}
