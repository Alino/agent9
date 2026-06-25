#pragma once
#include <string>
extern "C" long n9_pwrite(int, const void *, long, long long);
inline void emit(const std::string &s){ std::string t=s+"\n"; n9_pwrite(1,t.data(),(long)t.size(),-1); }
