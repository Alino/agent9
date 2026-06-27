#include <vector>
#include <algorithm>
#include <functional>
extern "C" long n9_pwrite(int,const void*,long,long long);
int main(){ std::vector<int> v={1,2,3,4,5,6}; int n=std::count_if(v.begin(),v.end(),[](int x){return x%2==0;});
  std::function<int(int)> f=[](int x){return x*x;}; int ok=(n==3)&&(f(4)==16);
  n9_pwrite(1,ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
