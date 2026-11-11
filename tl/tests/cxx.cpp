#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

struct Global { std::string text; Global() : text("static init ran") {} };
static Global global;
struct Counted { int *n; ~Counted() { ++*n; } };
static int destroyed;
thread_local Counted per_thread{&destroyed};
thread_local std::string tl_text = "thread local";

int main() {
    std::map<std::string, int> m{{"b", 2}, {"a", 1}};
    std::string order;
    for (auto &kv : m) order += kv.first;
    std::string caught = "none";
    try { throw std::runtime_error("c++ exception crossed loaded frames"); } catch (const std::exception &e) { caught = e.what(); }
    auto shared = std::make_shared<std::vector<int>>(std::vector<int>{1, 2, 3});
    std::thread t([] { (void)per_thread; tl_text += " (other thread)"; });
    t.join();
    printf("%s | %s | %s | size %zu | thread dtors %d | %s\n", global.text.c_str(), order.c_str(), caught.c_str(), shared->size(),
           destroyed, tl_text.c_str());
    printf("PASS cxx\n");
    return 0;
}
