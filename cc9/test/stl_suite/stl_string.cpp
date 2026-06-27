#include <string>
extern "C" long n9_pwrite(int,const void*,long,long long);
int main(){ std::string a="hello"; a+=" world"; std::string b=a.substr(6);
  int ok=(a.size()==11)&&(b=="world")&&(a.find("world")==6); n9_pwrite(1,ok?"PASS\n":"FAIL\n",5,-1); return ok?0:1; }
