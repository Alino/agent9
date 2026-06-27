#include <map>
#include <string>
extern "C" long n9_pwrite(int,const void*,long,long long);
int main(){ std::map<std::string,int> m; m["a"]=1; m["b"]=2; m["a"]=10;
  int ok=(m["a"]==10)&&(m["b"]==2)&&(m.size()==2)&&(m.count("c")==0); n9_pwrite(1,ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
