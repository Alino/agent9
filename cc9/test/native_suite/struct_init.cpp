extern "C" long n9_pwrite(int,const void*,long,long long);
struct P{ int a; int b; int c; };
int main(){ P arr[3]={{1,2,3},{4,5,6},{7,8,9}}; int s=0; for(auto&p:arr)s+=p.a+p.b+p.c;
  int ok=s==45; n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
