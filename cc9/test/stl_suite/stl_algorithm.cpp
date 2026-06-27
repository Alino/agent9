#include <vector>
#include <algorithm>
#include <numeric>
extern "C" long n9_pwrite(int,const void*,long,long long);
int main(){ std::vector<int> v(10); std::iota(v.begin(),v.end(),1);
  auto it=std::find(v.begin(),v.end(),7); int sum=std::accumulate(v.begin(),v.end(),0);
  int mx=*std::max_element(v.begin(),v.end()); int ok=(*it==7)&&(sum==55)&&(mx==10);
  n9_pwrite(1,ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
