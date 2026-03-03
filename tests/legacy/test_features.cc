// Test new features: checksum, config, metrics
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include "zerokv/checksum.h"
#include "zerokv/config.h"
#include "zerokv/metrics.h"

using namespace zerokv;

void test_checksum() {
    std::cout << "\n=== Test: Checksum ===" << std::endl;

    std::string data = "Hello ZeroKV!";
    uint32_t crc = CRC32::calculate(data);
    std::cout << "[CRC32] \"" << data << "\" = 0x" << std::hex << crc << std::dec << std::endl;

    // Verify
    auto result = IntegrityChecker::verify(data.data(), data.size(), crc);
    std::cout << "[Verify] " << (result.valid ? "PASSED" : "FAILED") << std::endl;

    // Tamper detection
    std::string tampered = "Hello ZeroKVX";
    result = IntegrityChecker::verify(tampered.data(), tampered.size(), crc);
    std::cout << "[Tamper] " << (result.valid ? "DETECTED FAILED" : "DETECTED OK") << std::endl;
}

void test_config() {
    std::cout << "\n=== Test: Config ===" << std::endl;

    Config cfg;

    // Set values
    cfg.set("server.port", 5000);
    cfg.set("server.max_memory", 1024);
    cfg.set("server.debug", true);
    cfg.set("server.name", "zerokv-node-1");

    // Get values
    int port = cfg.get<int>("server.port", 0);
    int max_mem = cfg.get<int>("server.max_memory", 0);
    bool debug = cfg.get<bool>("server.debug", false);
    std::string name = cfg.get<std::string>("server.name", "");

    std::cout << "[Get] port=" << port << ", max_memory=" << max_mem
              << ", debug=" << debug << ", name=" << name << std::endl;

    // Server config
    auto server_cfg = cfg.server();
    std::cout << "[Server] bind=" << server_cfg.bind_addr
              << ", port=" << server_cfg.port << std::endl;
}

void test_metrics() {
    std::cout << "\n=== Test: Metrics ===" << std::endl;

    auto& metrics = Metrics::instance();

    // Counters
    metrics.increment_counter("requests.total", 100);
    metrics.increment_counter("requests.get", 60);
    metrics.increment_counter("requests.put", 40);

    // Gauges
    metrics.set_gauge("memory.used_mb", 1024);
    metrics.set_gauge("connections.active", 16);

    // Histograms
    for (int i = 0; i < 100; i++) {
        metrics.record_histogram("request.latency_ms", i * 0.5 + 1);
    }

    // Print
    metrics.print();
}

void test_metrics_timer() {
    std::cout << "\n=== Test: Timer ===" << std::endl;

    // Simulate request timing
    for (int i = 0; i < 5; i++) {
        ZEROKV_TIMER("test.operation");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    Metrics::instance().print();
}

int main() {
    std::cout << "╔════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║          ZeroKV Feature Tests                          ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════╝" << std::endl;

    test_checksum();
    test_config();
    test_metrics();
    test_metrics_timer();

    std::cout << "\n=== All feature tests completed ===" << std::endl;
    return 0;
}
