extern "C" long n9_pwrite(int,const void*,long,long long);
int main(){ int a=3,b=4; auto byval=[a](int x){return a+x;}; auto byref=[&](int x){b+=x;return b;};
  int ok = (byval(10)==13) && (byref(6)==10) && (b==10);
  n9_pwrite(1, ok?"PASS\n":"FAIL\n", 5, -1); return ok?0:1; }
