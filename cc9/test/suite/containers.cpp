// EXPECT: 1 2 3 5 8 9 | a=1 b=2 c=3
#include "check.h"
#include <vector>
#include <map>
#include <algorithm>
int main(){
	std::vector<int> v{5,3,8,1,9,2}; std::sort(v.begin(),v.end());
	std::string r; for(int x:v){ if(!r.empty())r+=' '; r+=std::to_string(x); }
	std::map<std::string,int> m{{"c",3},{"a",1},{"b",2}};
	r+=" |"; for(auto&kv:m){ r+=' '; r+=kv.first; r+='='; r+=std::to_string(kv.second); }
	emit(r); return 0;
}
