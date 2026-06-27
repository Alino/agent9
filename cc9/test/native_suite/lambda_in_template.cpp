extern "C" long n9_pwrite(int,const void*,long,long long);
template<class F> int apply2(F f, int v){ return f(f(v)); }
int main(){ int ok=apply2([](int x){return x*3;}, 2)==18; n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
