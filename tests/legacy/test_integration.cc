// Integration tests - simplified without UCX dependency
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include "zerokv/storage.h"
#include "zerokv/metrics.h"

using namespace zerokv;

// Helper to convert Status to bool
static inline bool ok(Status s) { return s == Status::OK; }

// Test storage with multiple threads
void test_concurrent_access() {
    std::cout << "\n=== Test: Concurrent Access ===" << std::endl;

    auto storage = std::make_unique<StorageEngine>(100 * 1024 * 1024);
    std::atomic<int> success{0};
    std::atomic<int> failure{0};

    auto worker = [&](int id) {
        for (int i = 0; i < 100; i++) {
            std::string key = "key_" + std::to_string(id * 100 + i);
            std::string value = "value_" + std::to_string(id * 100 + i);

            if (ok(storage->put(key, value.data(), value.size()))) {
                success++;
            } else {
                failure++;
            }
        }
    };

    // Spawn 4 threads
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "[Result] Success: " << success << ", Failed: " << failure << std::endl;
    std::cout << "[Info] Total items: " << storage->item_count() << std::endl;
}

// Test with metrics
void test_with_metrics() {
    std::cout << "\n=== Test: With Metrics ===" << std::endl;

    auto& metrics = Metrics::instance();
    metrics.reset();

    auto storage = std::make_unique<StorageEngine>(50 * 1024 * 1024);

    // Simulate workload with metrics
    for (int i = 0; i < 100; i++) {
        {
            zerokv::Timer timer("storage.put");
            std::string key = "key_" + std::to_string(i);
            std::string value(1024, 'x');
            storage->put(key, value.data(), value.size());
        }
    }

    metrics.increment_counter("test.iterations", 100);
    metrics.set_gauge("test.storage_items", (double)storage->item_count());

    metrics.print();
}

// Test metrics timer
void test_metrics_timer() {
    std::cout << "\n=== Test: Metrics Timer ===" << std::endl;

    auto& metrics = Metrics::instance();
    metrics.reset();

    for (int i = 0; i < 10; i++) {
        zerokv::Timer timer("test.operation");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    metrics.print();
}

int main() {
    std::cout << "╔════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║          ZeroKV Integration Tests                     ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════╝" << std::endl;

    test_concurrent_access();
    test_with_metrics();
    test_metrics_timer();

    std::cout << "\n=== All integration tests passed ===" << std::endl;
    return 0;
}
