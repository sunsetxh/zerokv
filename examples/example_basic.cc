/**
 * ZeroKV C++ Usage Examples
 *
 * Compile:
 *   g++ -std=c++17 -O2 -I./include example_server.cc src/storage/storage.cc -o example_server
 *
 * Run:
 *   ./example_server
 */

#include <iostream>
#include <string>
#include <memory>
#include "zerokv/storage.h"
#include "zerokv/metrics.h"
#include "zerokv/config.h"
#include "zerokv/checksum.h"

using namespace zerokv;

int main() {
    std::cout << "=== ZeroKV C++ Examples ===" << std::endl;

    // Example 1: Basic storage usage
    std::cout << "\n--- Example 1: Basic Storage ---" << std::endl;
    {
        auto storage = std::make_unique<StorageEngine>(1024 * 1024); // 1MB

        // Put
        std::string key = "user:1001";
        std::string value = R"({"name": "Alice", "age": 30})";

        Status status = storage->put(key, value.data(), value.size());
        std::cout << "Put: " << (status == Status::OK ? "OK" : "FAILED") << std::endl;

        // Get
        char buffer[1024];
        size_t size = sizeof(buffer);
        status = storage->get(key, buffer, &size);

        if (status == Status::OK) {
            buffer[size] = '\0';
            std::cout << "Get: " << buffer << std::endl;
        }
    }

    // Example 2: Large value
    std::cout << "\n--- Example 2: Large Value ---" << std::endl;
    {
        auto storage = std::make_unique<StorageEngine>(10 * 1024 * 1024); // 10MB

        std::string key = "large_data";
        std::string value(1024 * 1024, 'X'); // 1MB

        auto start = std::chrono::high_resolution_clock::now();
        storage->put(key, value.data(), value.size());
        auto end = std::chrono::high_resolution_clock::now();

        auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        std::cout << "Put 1MB: " << us << " us" << std::endl;
    }

    // Example 3: Data integrity
    std::cout << "\n--- Example 3: Data Integrity ---" << std::endl;
    {
        std::string data = "Important data";
        uint32_t crc = CRC32::calculate(data);
        std::cout << "CRC32: 0x" << std::hex << crc << std::dec << std::endl;

        auto result = IntegrityChecker::verify(data.data(), data.size(), crc);
        std::cout << "Verify: " << (result.valid ? "OK" : "FAILED") << std::endl;
    }

    // Example 4: Configuration
    std::cout << "\n--- Example 4: Configuration ---" << std::endl;
    {
        Config cfg;
        cfg.set("server.port", 5000);
        cfg.set("server.max_memory", 1024);
        cfg.set("server.debug", true);

        int port = cfg.get<int>("server.port", 0);
        bool debug = cfg.get<bool>("server.debug", false);

        std::cout << "Port: " << port << ", Debug: " << (debug ? "true" : "false") << std::endl;
    }

    // Example 5: Metrics
    std::cout << "\n--- Example 5: Metrics ---" << std::endl;
    {
        auto& metrics = Metrics::instance();

        metrics.increment_counter("requests.total", 100);
        metrics.set_gauge("memory.used_mb", 512);
        metrics.record_histogram("request.latency_ms", 10.5);

        metrics.print();
    }

    std::cout << "\n=== All examples completed ===" << std::endl;
    return 0;
}
