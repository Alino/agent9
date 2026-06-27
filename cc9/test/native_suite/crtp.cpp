extern "C" long n9_pwrite(int,const void*,long,long long);
template<class D> struct Shape{ int area(){ return static_cast<D*>(this)->areaImpl(); } };
struct Sq: Shape<Sq>{ int s; Sq(int x):s(x){} int areaImpl(){return s*s;} };
int main(){ Sq q(5); int ok=q.area()==25; n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
