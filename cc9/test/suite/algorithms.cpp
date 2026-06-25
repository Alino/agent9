// EXPECT: sum=55 max=10 count2=1 evens=5
#include "check.h"
#include <vector>
#include <algorithm>
#include <numeric>
int main(){
	std::vector<int> v; for(int i=1;i<=10;i++) v.push_back(i);
	int sum=std::accumulate(v.begin(),v.end(),0);
	int mx=*std::max_element(v.begin(),v.end());
	long c2=std::count(v.begin(),v.end(),2);
	long ev=std::count_if(v.begin(),v.end(),[](int x){return x%2==0;});
	emit("sum="+std::to_string(sum)+" max="+std::to_string(mx)+" count2="+std::to_string(c2)+" evens="+std::to_string(ev));
	return 0;
}
