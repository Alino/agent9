#include <tuple>
extern "C" long n9_pwrite(int,const void*,long,long long);
int main(){ auto t=std::make_tuple(1,2.5,3); auto [a,b,c]=t;
  int ok=(a==1)&&(b==2.5)&&(c==3)&&(std::get<0>(t)==1); n9_pwrite(1,ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
