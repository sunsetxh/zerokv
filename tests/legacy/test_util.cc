// Test utilities
#include <iostream>
#include "zerokv/util.h"

using namespace zerokv;

void test_string() {
    std::cout << "\n=== Test: String Utils ===" << std::endl;

    std::string s = "  hello world  ";
    std::cout << "[trim] '" << str::trim(s) << "'" << std::endl;

    std::string split = "a,b,c,d";
    auto parts = str::split(split, ',');
    std::cout << "[split] " << parts.size() << " parts" << std::endl;

    std::cout << "[join] " << str::join(parts, "-") << std::endl;
    std::cout << "[lower] " << str::to_upper("Hello") << std::endl;
    std::cout << "[upper] " << str::to_lower("Hello") << std::endl;
}

void test_time() {
    std::cout << "\n=== Test: Time Utils ===" << std::endl;

    std::cout << "[now_ms] " << time::now_ms() << std::endl;
    std::cout << "[format] " << time::format_duration(1500) << std::endl;
    std::cout << "[format] " << time::format_duration(65000) << std::endl;
}

void test_random() {
    std::cout << "\n=== Test: Random Utils ===" << std::endl;

    std::cout << "[random str] " << random::generate_string(16) << std::endl;
    std::cout << "[random int] " << random::generate_int(1, 100) << std::endl;
}

void test_bytes() {
    std::cout << "\n=== Test: Bytes Utils ===" << std::endl;

    std::cout << "[format 1024] " << bytes::format(1024) << std::endl;
    std::cout << "[format 1048576] " << bytes::format(1048576) << std::endl;
    std::cout << "[format 1073741824] " << bytes::format(1073741824) << std::endl;

    std::cout << "[parse 1KB] " << bytes::parse("1KB") << std::endl;
    std::cout << "[parse 1MB] " << bytes::parse("1MB") << std::endl;
}

int main() {
    std::cout << "╔═══════════════════════════════════════╗" << std::endl;
    std::cout << "║  ZeroKV Utils Test               ║" << std::endl;
    std::cout << "╚═══════════════════════════════════════╝" << std::endl;

    test_string();
    test_time();
    test_random();
    test_bytes();

    std::cout << "\n=== All utils tests completed ===" << std::endl;
    return 0;
}
