#include <type_traits>
extern "C" long n9_pwrite(int,const void*,long,long long);
int main(){ int ok = std::is_integral<int>::value && !std::is_integral<double>::value
  && std::is_same<int,std::remove_const<const int>::type>::value && std::is_pointer<int*>::value;
  n9_pwrite(1,ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
