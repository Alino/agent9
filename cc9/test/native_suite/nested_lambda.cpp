extern "C" long n9_pwrite(int,const void*,long,long long);
int main(){ auto adder=[](int x){ return [x](int y){ return x+y; }; }; auto add5=adder(5);
  int ok=add5(7)==12; n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
