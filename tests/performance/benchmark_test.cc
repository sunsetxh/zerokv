// Performance Benchmark Tests
#include <gtest/gtest.h>
#include "zerokv/storage.h"
#include <chrono>
#include <vector>
#include <random>
#include <iostream>

using namespace zerokv;

// Benchmark test for storage put
TEST(BenchmarkTest, Put_1KB) {
    StorageEngine storage(1024ULL * 1024 * 1024);
    std::string value(1024, 'x');

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < 10000; ++i) {
        std::string key = "key_" + std::to_string(i);
        storage.put(key, value.data(), value.size());
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "Put 1KB x 10000: " << duration.count() << "ms" << std::endl;
    EXPECT_LT(duration.count(), 1000); // Should complete in < 1 second
}

// Benchmark test for storage get
TEST(BenchmarkTest, Get_Random) {
    StorageEngine storage(1024ULL * 1024 * 1024);

    // Pre-populate
    for (int i = 0; i < 1000; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string value = "value_" + std::to_string(i);
        storage.put(key, value.data(), value.size());
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, 999);

    std::vector<char> buffer(1024);
    size_t size = buffer.size();

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < 10000; ++i) {
        int idx = dist(gen);
        std::string key = "key_" + std::to_string(idx);
        storage.get(key, buffer.data(), &size);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "Get random x 10000: " << duration.count() << "ms" << std::endl;
    EXPECT_LT(duration.count(), 1000);
}

// Large value benchmark
TEST(BenchmarkTest, LargeValue_1MB) {
    StorageEngine storage(1024ULL * 1024 * 1024);
    std::string value(1024 * 1024, 'L'); // 1MB

    auto start = std::chrono::high_resolution_clock::now();

    storage.put("large_key", value.data(), value.size());

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "Put 1MB: " << duration.count() << "ms" << std::endl;
}
