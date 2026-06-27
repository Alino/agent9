extern "C" long n9_pwrite(int,const void*,long,long long);
constexpr int fact(int n){ return n<=1?1:n*fact(n-1); }
int main(){ constexpr int f=fact(5); static_assert(f==120,"x"); int ok=(f==120);
  n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
