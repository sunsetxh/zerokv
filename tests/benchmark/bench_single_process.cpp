/// @file bench_single_process.cpp
/// @brief Single-process performance benchmark (no network required)

#include <axon/config.h>
#include <axon/worker.h>
#include <axon/endpoint.h>
#include <axon/memory.h>

#include <chrono>
#include <iostream>
#include <vector>
#include <cstring>

using namespace axon;

// Benchmark worker creation
void benchmark_worker_creation() {
    auto config = Config::builder().set_transport("tcp").build();
    auto context = Context::create(config);

    const int iterations = 100;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        auto worker = Worker::create(context);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "Worker creation: "
              << iterations << " iterations in "
              << duration.count() << " us ("
              << (duration.count() / iterations) << " us/iter)"
              << std::endl;
}

// Benchmark worker address retrieval
void benchmark_worker_address() {
    auto config = Config::builder().set_transport("tcp").build();
    auto context = Context::create(config);
    auto worker = Worker::create(context);

    const int iterations = 1000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        auto addr = worker->address();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "Worker address: "
              << iterations << " iterations in "
              << duration.count() << " us ("
              << (duration.count() / iterations) << " us/iter)"
              << std::endl;
}

// Benchmark memory region operations
void benchmark_memory_region() {
    auto config = Config::builder().set_transport("tcp").build();
    auto context = Context::create(config);

    const size_t buffer_size = 1024 * 1024;  // 1MB
    std::vector<char> buffer(buffer_size, 0);

    const int iterations = 100;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        auto region = MemoryRegion::register_mem(context, buffer.data(), buffer_size);
        auto rkey = region->remote_key();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "Memory region registration + rkey: "
              << iterations << " iterations in "
              << duration.count() << " us ("
              << (duration.count() / iterations) << " us/iter)"
              << std::endl;
}

// Benchmark progress calls
void benchmark_progress() {
    auto config = Config::builder().set_transport("tcp").build();
    auto context = Context::create(config);
    auto worker = Worker::create(context);

    const int iterations = 10000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        worker->progress();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "Progress calls: "
              << iterations << " iterations in "
              << duration.count() << " us ("
              << (duration.count() / iterations) << " us/iter)"
              << std::endl;
}

int main() {
    std::cout << "=== AXON Single-Process Performance Benchmark ===" << std::endl;
    std::cout << std::endl;

    std::cout << "--- Worker Creation ---" << std::endl;
    benchmark_worker_creation();
    std::cout << std::endl;

    std::cout << "--- Worker Address Retrieval ---" << std::endl;
    benchmark_worker_address();
    std::cout << std::endl;

    std::cout << "--- Memory Region Operations ---" << std::endl;
    benchmark_memory_region();
    std::cout << std::endl;

    std::cout << "--- Progress Calls ---" << std::endl;
    benchmark_progress();
    std::cout << std::endl;

    std::cout << "=== Benchmark Complete ===" << std::endl;
    return 0;
}
