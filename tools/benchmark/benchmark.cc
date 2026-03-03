#include <iostream>
#include <chrono>
#include <random>
#include <vector>
#include <string>
#include <cstring>
#include "zerokv/client.h"
#include "zerokv/storage.h"

using namespace zerokv;

struct BenchmarkConfig {
    std::vector<std::string> servers;
    int num_ops = 10000;
    int value_size = 1024;
    int num_threads = 1;
    bool random_size = false;
};

struct BenchmarkResult {
    double latency_avg_us;
    double latency_p50_us;
    double latency_p99_us;
    double throughput_ops_sec;
};

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -s <servers>   Comma-separated server list (default: localhost:5000)\n"
              << "  -n <num>       Number of operations (default: 10000)\n"
              << "  -z <size>      Value size in bytes (default: 1024)\n"
              << "  -t <threads>   Number of threads (default: 1)\n"
              << "  -r             Random value sizes (1KB-1MB)\n"
              << "  -h             Show this help\n";
}

BenchmarkConfig parse_args(int argc, char** argv) {
    BenchmarkConfig config;
    config.servers = {"localhost:5000"};

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-s" && i + 1 < argc) {
            std::string servers = argv[++i];
            config.servers.clear();
            size_t start = 0, end = 0;
            while ((end = servers.find(',', start)) != std::string::npos) {
                config.servers.push_back(servers.substr(start, end - start));
                start = end + 1;
            }
            config.servers.push_back(servers.substr(start));
        } else if (arg == "-n" && i + 1 < argc) {
            config.num_ops = std::stoi(argv[++i]);
        } else if (arg == "-z" && i + 1 < argc) {
            config.value_size = std::stoi(argv[++i]);
        } else if (arg == "-t" && i + 1 < argc) {
            config.num_threads = std::stoi(argv[++i]);
        } else if (arg == "-r") {
            config.random_size = true;
        } else if (arg == "-h") {
            print_usage(argv[0]);
            exit(0);
        }
    }

    return config;
}

BenchmarkResult run_benchmark(const BenchmarkConfig& config) {
    // Create storage for local test (if no servers)
    auto storage = std::make_unique<StorageEngine>(1024 * 1024 * 1024);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> size_dist(1024, 1024 * 1024);

    std::vector<double> latencies;
    latencies.reserve(config.num_ops);

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < config.num_ops; ++i) {
        auto op_start = std::chrono::high_resolution_clock::now();

        // Generate key and value
        std::string key = "key_" + std::to_string(i);
        int value_size = config.random_size ? size_dist(gen) : config.value_size;
        std::string value(value_size, 'x');

        // Put operation
        storage->put(key, value.data(), value_size);

        // Get operation
        std::vector<char> buffer(value_size);
        size_t size = buffer.size();
        storage->get(key, buffer.data(), &size);

        auto op_end = std::chrono::high_resolution_clock::now();
        double latency_us = std::chrono::duration<double, std::micro>(
            op_end - op_start).count();
        latencies.push_back(latency_us);
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double total_sec = std::chrono::duration<double>(end_time - start_time).count();

    // Calculate statistics
    std::sort(latencies.begin(), latencies.end());

    BenchmarkResult result;
    result.latency_avg_us = 0;
    for (double l : latencies) result.latency_avg_us += l;
    result.latency_avg_us /= latencies.size();

    result.latency_p50_us = latencies[latencies.size() * 50 / 100];
    result.latency_p99_us = latencies[latencies.size() * 99 / 100];
    result.throughput_ops_sec = config.num_ops * 2 / total_sec; // put + get

    return result;
}

void print_result(const BenchmarkResult& result) {
    std::cout << "\n=== Benchmark Results ===\n"
              << "Average Latency: " << result.latency_avg_us << " us\n"
              << "P50 Latency:     " << result.latency_p50_us << " us\n"
              << "P99 Latency:     " << result.latency_p99_us << " us\n"
              << "Throughput:      " << result.throughput_ops_sec << " ops/sec\n";
}

int main(int argc, char** argv) {
    auto config = parse_args(argc, argv);

    std::cout << "=== ZeroKV Benchmark ===\n"
              << "Operations: " << config.num_ops << "\n"
              << "Value Size:  " << config.value_size << " bytes\n"
              << "Threads:     " << config.num_threads << "\n"
              << "Random Size: " << (config.random_size ? "yes" : "no") << "\n";

    auto result = run_benchmark(config);
    print_result(result);

    return 0;
}
