#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <string>
#include <algorithm>
#include <cmath>
#include <optional>
extern "C" long n9_pwrite(int, const void *, long, long long);
extern "C" void n9_exits(const char *);

extern "C" void _start()
{
	std::string r;
	std::vector<int> v{5, 3, 8, 1, 9, 2};
	std::sort(v.begin(), v.end());
	r += "sorted:";
	for (int x : v) { r += ' '; r += std::to_string(x); }
	std::map<std::string, int> m{{"a", 1}, {"b", 2}};
	r += " map:";
	for (auto &kv : m) { r += ' '; r += kv.first; r += '='; r += std::to_string(kv.second); }
	std::unordered_map<std::string, int> um{{"x", 10}, {"y", 20}};
	r += " um[x]="; r += std::to_string(um["x"]);
	std::set<int> s{3, 1, 2, 1};
	r += " setsize:"; r += std::to_string((int)s.size());
	r += " sqrt2*1e3:"; r += std::to_string((int)(std::sqrt(2.0) * 1000));
	std::optional<int> o = 42;
	r += " opt:"; r += std::to_string(o.value_or(0));
	r += '\n';
	n9_pwrite(1, r.data(), (long)r.size(), -1);
	n9_exits(0);
}
