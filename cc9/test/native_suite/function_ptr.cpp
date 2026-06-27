extern "C" long n9_pwrite(int,const void*,long,long long);
static int inc(int x){return x+1;} static int dbl(int x){return x*2;}
int main(){ int(*fp[])(int)={inc,dbl}; int ok=(fp[0](5)==6)&&(fp[1](5)==10);
  n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
