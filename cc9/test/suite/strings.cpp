// EXPECT: hello world|len=11|sub=world|find=6|num=12345
#include "check.h"
int main(){
	std::string s="hello"; s+=" "; s+="world";
	std::string r=s+"|len="+std::to_string(s.size());
	r+="|sub="+s.substr(6);
	r+="|find="+std::to_string(s.find("world"));
	r+="|num="+std::to_string(12345);
	emit(r); return 0;
}
