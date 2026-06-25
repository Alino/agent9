// Real third-party C++ on 9front: nlohmann/json parse -> manipulate -> serialize.
#define JSON_NOEXCEPTION 1
#define JSON_NO_IO 1
#include "../vendor/json.hpp"
extern "C" long n9_pwrite(int, const void *, long, long long);
extern "C" void n9_exits(const char *);
using json = nlohmann::json;

int main()
{
	json j = json::parse(R"({"os":"9front","answer":40,"langs":["c","go","js"]})");
	j["answer"] = j["answer"].get<int>() + 2;      // 42
	j["langs"].push_back("c++");
	j["nested"] = { {"vec_works", true}, {"count", 8} };
	std::string out = j.dump();
	n9_pwrite(1, out.data(), (long)out.size(), -1);
	n9_pwrite(1, "\n", 1, -1);
	return 0;
}
