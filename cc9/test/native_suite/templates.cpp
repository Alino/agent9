extern "C" long n9_pwrite(int,const void*,long,long long);
template<class T> T mymax(T a, T b){ return a>b?a:b; }
template<class T> struct Box{ T v; T get(){return v;} };
int main(){ Box<int> b{42}; int ok=(mymax(3,7)==7)&&(mymax(2.0,1.0)==2.0)&&(b.get()==42);
  n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
