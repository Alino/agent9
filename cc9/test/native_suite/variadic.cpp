extern "C" long n9_pwrite(int,const void*,long,long long);
template<class... A> int sum(A... a){ return (a + ... + 0); }
int main(){ int ok = sum(1,2,3,4,5)==15 && sum()==0; n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
