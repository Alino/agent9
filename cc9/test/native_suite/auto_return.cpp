extern "C" long n9_pwrite(int,const void*,long,long long);
auto square(int x){ return x*x; }
int main(){ int ok=square(9)==81; n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
