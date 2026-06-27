extern "C" long n9_pwrite(int,const void*,long,long long);
struct V{ int x,y; V operator+(const V&o)const{return {x+o.x,y+o.y};} bool operator==(const V&o)const{return x==o.x&&y==o.y;} };
int main(){ V a{1,2},b{3,4}; int ok=(a+b)==V{4,6}; n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
