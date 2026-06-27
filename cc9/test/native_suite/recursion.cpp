extern "C" long n9_pwrite(int,const void*,long,long long);
static long fib(int n){ return n<2? n : fib(n-1)+fib(n-2); }
int main(){ int ok = fib(15)==610; n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
