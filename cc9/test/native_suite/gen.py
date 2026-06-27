#!/usr/bin/env python3
# Generates a C++ language-feature conformance suite (no STL includes, so no on-box
# header staging needed). Each test is self-checking: main() returns 0 and prints
# "PASS\n" on success, else prints "FAIL\n" and returns 1. The native cc9 clang
# (on 9front) compiles each; we then build+run to verify the output is correct.
import os, sys
TESTS = {
"lambda_capture": r'''
extern "C" long n9_pwrite(int,const void*,long,long long);
int main(){ int a=3,b=4; auto byval=[a](int x){return a+x;}; auto byref=[&](int x){b+=x;return b;};
  int ok = (byval(10)==13) && (byref(6)==10) && (b==10);
  n9_pwrite(1, ok?"PASS\n":"FAIL\n", 5, -1); return ok?0:1; }''',
"generic_lambda": r'''
extern "C" long n9_pwrite(int,const void*,long,long long);
int main(){ auto add=[](auto x, auto y){return x+y;};
  int ok = (add(2,3)==5) && (add(1.5,2.5)==4.0); n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }''',
"recursion": r'''
extern "C" long n9_pwrite(int,const void*,long,long long);
static long fib(int n){ return n<2? n : fib(n-1)+fib(n-2); }
int main(){ int ok = fib(15)==610; n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }''',
"templates": r'''
extern "C" long n9_pwrite(int,const void*,long,long long);
template<class T> T mymax(T a, T b){ return a>b?a:b; }
template<class T> struct Box{ T v; T get(){return v;} };
int main(){ Box<int> b{42}; int ok=(mymax(3,7)==7)&&(mymax(2.0,1.0)==2.0)&&(b.get()==42);
  n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }''',
"variadic": r'''
extern "C" long n9_pwrite(int,const void*,long,long long);
template<class... A> int sum(A... a){ return (a + ... + 0); }
int main(){ int ok = sum(1,2,3,4,5)==15 && sum()==0; n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }''',
"constexpr": r'''
extern "C" long n9_pwrite(int,const void*,long,long long);
constexpr int fact(int n){ return n<=1?1:n*fact(n-1); }
int main(){ constexpr int f=fact(5); static_assert(f==120,"x"); int ok=(f==120);
  n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }''',
"auto_return": r'''
extern "C" long n9_pwrite(int,const void*,long,long long);
auto square(int x){ return x*x; }
int main(){ int ok=square(9)==81; n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }''',
"virtual_rtti": r'''
extern "C" long n9_pwrite(int,const void*,long,long long);
struct Base{ virtual int f(){return 1;} virtual ~Base(){} };
struct Derived: Base{ int f() override {return 2;} };
int main(){ Base* p=new Derived(); int ok=(p->f()==2); delete p;
  n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }''',
"crtp": r'''
extern "C" long n9_pwrite(int,const void*,long,long long);
template<class D> struct Shape{ int area(){ return static_cast<D*>(this)->areaImpl(); } };
struct Sq: Shape<Sq>{ int s; Sq(int x):s(x){} int areaImpl(){return s*s;} };
int main(){ Sq q(5); int ok=q.area()==25; n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }''',
"operator_overload": r'''
extern "C" long n9_pwrite(int,const void*,long,long long);
struct V{ int x,y; V operator+(const V&o)const{return {x+o.x,y+o.y};} bool operator==(const V&o)const{return x==o.x&&y==o.y;} };
int main(){ V a{1,2},b{3,4}; int ok=(a+b)==V{4,6}; n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }''',
"nested_lambda": r'''
extern "C" long n9_pwrite(int,const void*,long,long long);
int main(){ auto adder=[](int x){ return [x](int y){ return x+y; }; }; auto add5=adder(5);
  int ok=add5(7)==12; n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }''',
"lambda_in_template": r'''
extern "C" long n9_pwrite(int,const void*,long,long long);
template<class F> int apply2(F f, int v){ return f(f(v)); }
int main(){ int ok=apply2([](int x){return x*3;}, 2)==18; n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }''',
"enum_switch": r'''
extern "C" long n9_pwrite(int,const void*,long,long long);
enum class Color{Red,Green,Blue};
static int val(Color c){ switch(c){case Color::Red:return 1;case Color::Green:return 2;case Color::Blue:return 3;} return 0; }
int main(){ int ok=val(Color::Green)==2 && val(Color::Blue)==3; n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }''',
"function_ptr": r'''
extern "C" long n9_pwrite(int,const void*,long,long long);
static int inc(int x){return x+1;} static int dbl(int x){return x*2;}
int main(){ int(*fp[])(int)={inc,dbl}; int ok=(fp[0](5)==6)&&(fp[1](5)==10);
  n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }''',
"struct_init": r'''
extern "C" long n9_pwrite(int,const void*,long,long long);
struct P{ int a; int b; int c; };
int main(){ P arr[3]={{1,2,3},{4,5,6},{7,8,9}}; int s=0; for(auto&p:arr)s+=p.a+p.b+p.c;
  int ok=s==45; n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }''',
}
if __name__=="__main__":
    d=os.path.dirname(os.path.abspath(__file__))
    for name,src in TESTS.items():
        open(os.path.join(d,name+".cpp"),"w").write(src.lstrip()+"\n")
    print(len(TESTS),"tests written to",d)
