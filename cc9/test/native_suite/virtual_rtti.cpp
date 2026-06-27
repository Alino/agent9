extern "C" long n9_pwrite(int,const void*,long,long long);
struct Base{ virtual int f(){return 1;} virtual ~Base(){} };
struct Derived: Base{ int f() override {return 2;} };
int main(){ Base* p=new Derived(); int ok=(p->f()==2); delete p;
  n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
