#include <optional>
extern "C" long n9_pwrite(int,const void*,long,long long);
static std::optional<int> half(int x){ if(x%2)return std::nullopt; return x/2; }
int main(){ auto a=half(10); auto b=half(7); int ok=a.has_value()&&(*a==5)&&!b.has_value()&&(b.value_or(-1)==-1);
  n9_pwrite(1,ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
