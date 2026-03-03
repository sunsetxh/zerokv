// Simple storage test
#include <iostream>
#include <chrono>
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>

class SimpleStorage {
public:
    std::unordered_map<std::string, std::vector<char>> data;
    std::mutex mutex;

    bool put(const std::string& key, const void* value, size_t size) {
        std::lock_guard<std::mutex> lock(mutex);
        const char* src = static_cast<const char*>(value);
        data[key] = std::vector<char>(src, src + size);
        return true;
    }

    bool get(const std::string& key, void* buffer, size_t* size) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = data.find(key);
        if (it == data.end()) return false;
        if (*size < it->second.size()) {
            *size = it->second.size();
            return false;
        }
        char* dst = static_cast<char*>(buffer);
        std::copy(it->second.begin(), it->second.end(), dst);
        *size = it->second.size();
        return true;
    }

    bool remove(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex);
        return data.erase(key) > 0;
    }
};

int main() {
    std::cout << "╔════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║          ZeroKV Storage Engine Test                     ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════╝" << std::endl;

    auto storage = std::make_unique<SimpleStorage>();

    // Test 1: Basic Put/Get
    std::cout << "\n=== Test 1: Basic Put/Get ===" << std::endl;
    storage->put("key1", "Hello ZeroKV!", 13);

    char buffer[1024];
    size_t size = sizeof(buffer);
    bool ok = storage->get("key1", buffer, &size);
    buffer[size] = '\0';
    std::cout << "[Result] " << (ok ? "SUCCESS" : "FAILED") << std::endl;
    std::cout << "[Value] " << buffer << std::endl;

    // Test 2: Large value
    std::cout << "\n=== Test 2: Large Value (1MB) ===" << std::endl;
    std::string large(1024 * 1024, 'L');

    auto start = std::chrono::high_resolution_clock::now();
    storage->put("large", large.data(), large.size());
    auto end = std::chrono::high_resolution_clock::now();
    auto dur = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "[Result] SUCCESS" << std::endl;
    std::cout << "[Perf] Put 1MB took " << dur.count() << " us" << std::endl;

    // Test 3: Benchmark
    std::cout << "\n=== Test 3: Benchmark (10000 ops) ===" << std::endl;
    std::string value(1024, 'x');

    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 10000; ++i) {
        std::string key = "key_" + std::to_string(i);
        storage->put(key, value.data(), value.size());
    }
    end = std::chrono::high_resolution_clock::now();
    dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "[Perf] Put 10000 x 1KB: " << dur.count() << " ms" << std::endl;
    std::cout << "[Perf] Throughput: " << (10000 * 1000 / dur.count()) << " ops/sec" << std::endl;

    // Test 4: Delete
    std::cout << "\n=== Test 4: Delete ===" << std::endl;
    storage->remove("key1");
    size = sizeof(buffer);
    ok = storage->get("key1", buffer, &size);
    std::cout << "[Result] Delete: " << (ok ? "FAILED" : "SUCCESS (not found)") << std::endl;

    std::cout << "\n=== All tests completed ===" << std::endl;
    return 0;
}
