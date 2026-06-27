extern "C" long n9_pwrite(int,const void*,long,long long);
int main(){ auto add=[](auto x, auto y){return x+y;};
  int ok = (add(2,3)==5) && (add(1.5,2.5)==4.0); n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
