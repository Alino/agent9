extern "C" long n9_pwrite(int,const void*,long,long long);
enum class Color{Red,Green,Blue};
static int val(Color c){ switch(c){case Color::Red:return 1;case Color::Green:return 2;case Color::Blue:return 3;} return 0; }
int main(){ int ok=val(Color::Green)==2 && val(Color::Blue)==3; n9_pwrite(1, ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
