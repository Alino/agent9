#include <vector>
#include <algorithm>
extern "C" long n9_pwrite(int,const void*,long,long long);
int main(){ std::vector<int> v={5,3,1,4,2}; std::sort(v.begin(),v.end()); int s=0; for(int x:v)s+=x;
  int ok=v[0]==1&&v[4]==5&&v.size()==5&&s==15; n9_pwrite(1,ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
