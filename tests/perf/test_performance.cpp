/**
 * Copyright (c) 2025. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file test_performance.cpp
 * @brief Performance benchmark tests for ZeroKV
 */

#include <gtest/gtest.h>
#include <zerokv/ucx_control_client.h>
#include <zerokv/ucx_control_server.h>
#include <zerokv/logger.h>
#include <thread>
#include <chrono>
#include <vector>
#include <numeric>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <random>

using namespace zerokv;

class PerformanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Disable logger for tests
        zerokv::LogManager::Instance().SetLevel(zerokv::LogLevel::ERROR);
        // Allocate unique port for each test
        test_port_ = AllocateUniquePort();
    }

    void TearDown() override {
        // Give UCX time to properly release the port
        // Real UCX environment needs significant time for port release
        // TCP TIME_WAIT can last up to 60 seconds in Linux
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    }

    static uint16_t AllocateUniquePort() {
        // Use random port between 40000 and 60000
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<uint16_t> dist(40000, 60000);
        return dist(gen);
    }

    void StartServer() {
        server_config_.listen_address = "127.0.0.1";
        server_config_.listen_port = test_port_;
        server_config_.use_rdma = false;
        server_config_.max_connections = 10;
        server_config_.max_kv_size = 1024 * 1024;

        server_ = std::make_unique<UCXControlServer>(server_config_);
        ASSERT_TRUE(server_->Initialize());
        ASSERT_TRUE(server_->Start());

        // Give server time to start listening
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void StopServer() {
        if (server_) {
            server_->Stop();
            server_.reset();
        }
    }

    void ConnectClient() {
        client_config_.use_rdma = false;
        client_config_.connect_timeout_ms = 2000;
        client_config_.request_timeout_ms = 5000;
        client_config_.max_retries = 3;

        client_ = std::make_unique<UCXControlClient>(client_config_);
        ASSERT_TRUE(client_->Initialize());
        ASSERT_TRUE(client_->Connect("127.0.0.1", test_port_));
        ASSERT_TRUE(client_->IsConnected());
    }

    void DisconnectClient() {
        if (client_) {
            client_->Disconnect();
            client_.reset();
        }
    }

    // Helper function to calculate statistics
    struct Stats {
        double mean;
        double median;
        double min;
        double max;
        double percentile_95;
        double percentile_99;
    };

    static Stats CalculateStats(std::vector<double> samples) {
        if (samples.empty()) {
            return {0, 0, 0, 0, 0, 0};
        }

        std::sort(samples.begin(), samples.end());

        Stats stats;
        stats.min = samples.front();
        stats.max = samples.back();
        stats.mean = std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();
        stats.median = samples[samples.size() / 2];
        stats.percentile_95 = samples[static_cast<size_t>(samples.size() * 0.95)];
        stats.percentile_99 = samples[static_cast<size_t>(samples.size() * 0.99)];

        return stats;
    }

    static std::string FormatLatency(double microseconds) {
        std::ostringstream oss;
        if (microseconds < 1000.0) {
            oss << std::fixed << std::setprecision(2) << microseconds << " μs";
        } else if (microseconds < 1000000.0) {
            oss << std::fixed << std::setprecision(2) << (microseconds / 1000.0) << " ms";
        } else {
            oss << std::fixed << std::setprecision(2) << (microseconds / 1000000.0) << " s";
        }
        return oss.str();
    }

    uint16_t test_port_;
    UCXServerConfig server_config_;
    UCXClientConfig client_config_;
    std::unique_ptr<UCXControlServer> server_;
    std::unique_ptr<UCXControlClient> client_;
};

//==============================================================================
// Latency Benchmarks
//==============================================================================

TEST_F(PerformanceTest, PutLatencyBenchmark) {
    StartServer();
    ConnectClient();

    const int num_iterations = 1000;
    std::vector<double> latencies;

    for (int i = 0; i < num_iterations; ++i) {
        PutRequest put_req;
        put_req.set_key("perf_key_" + std::to_string(i));
        put_req.set_dev_ptr(0x1000 + i);
        put_req.set_size(1024);
        put_req.set_data_type(DataType::FP32);
        put_req.set_client_id("perf_client");

        auto start = std::chrono::high_resolution_clock::now();
        auto result = client_->Put(put_req);
        auto end = std::chrono::high_resolution_clock::now();

        ASSERT_EQ(result.status, RPCStatus::SUCCESS);

        double latency_us = std::chrono::duration<double, std::micro>(end - start).count();
        latencies.push_back(latency_us);
    }

    Stats stats = CalculateStats(latencies);

    std::cout << "\n=== PUT Latency Benchmark (" << num_iterations << " operations) ===" << std::endl;
    std::cout << "  Mean:   " << FormatLatency(stats.mean) << std::endl;
    std::cout << "  Median: " << FormatLatency(stats.median) << std::endl;
    std::cout << "  Min:    " << FormatLatency(stats.min) << std::endl;
    std::cout << "  Max:    " << FormatLatency(stats.max) << std::endl;
    std::cout << "  P95:    " << FormatLatency(stats.percentile_95) << std::endl;
    std::cout << "  P99:    " << FormatLatency(stats.percentile_99) << std::endl;

    DisconnectClient();
    StopServer();
}

TEST_F(PerformanceTest, GetLatencyBenchmark) {
    StartServer();
    ConnectClient();

    // First, populate some keys
    const int num_keys = 100;
    for (int i = 0; i < num_keys; ++i) {
        PutRequest put_req;
        put_req.set_key("perf_key_" + std::to_string(i));
        put_req.set_dev_ptr(0x1000 + i);
        put_req.set_size(1024);
        put_req.set_data_type(DataType::FP32);
        put_req.set_client_id("perf_client");

        ASSERT_EQ(client_->Put(put_req).status, RPCStatus::SUCCESS);
    }

    // Benchmark GET operations
    const int num_iterations = 1000;
    std::vector<double> latencies;

    for (int i = 0; i < num_iterations; ++i) {
        int key_idx = i % num_keys;

        GetRequest get_req;
        get_req.set_key("perf_key_" + std::to_string(key_idx));
        get_req.set_client_id("perf_client");

        auto start = std::chrono::high_resolution_clock::now();
        auto result = client_->Get(get_req);
        auto end = std::chrono::high_resolution_clock::now();

        ASSERT_EQ(result.status, RPCStatus::SUCCESS);

        double latency_us = std::chrono::duration<double, std::micro>(end - start).count();
        latencies.push_back(latency_us);
    }

    Stats stats = CalculateStats(latencies);

    std::cout << "\n=== GET Latency Benchmark (" << num_iterations << " operations) ===" << std::endl;
    std::cout << "  Mean:   " << FormatLatency(stats.mean) << std::endl;
    std::cout << "  Median: " << FormatLatency(stats.median) << std::endl;
    std::cout << "  Min:    " << FormatLatency(stats.min) << std::endl;
    std::cout << "  Max:    " << FormatLatency(stats.max) << std::endl;
    std::cout << "  P95:    " << FormatLatency(stats.percentile_95) << std::endl;
    std::cout << "  P99:    " << FormatLatency(stats.percentile_99) << std::endl;

    DisconnectClient();
    StopServer();
}

TEST_F(PerformanceTest, DeleteLatencyBenchmark) {
    StartServer();
    ConnectClient();

    const int num_iterations = 100;
    std::vector<double> latencies;

    for (int i = 0; i < num_iterations; ++i) {
        // First put a key
        PutRequest put_req;
        put_req.set_key("delete_perf_key_" + std::to_string(i));
        put_req.set_dev_ptr(0x1000 + i);
        put_req.set_size(1024);
        put_req.set_data_type(DataType::FP32);
        put_req.set_client_id("perf_client");

        ASSERT_EQ(client_->Put(put_req).status, RPCStatus::SUCCESS);

        // Then benchmark DELETE
        DeleteRequest del_req;
        del_req.set_key("delete_perf_key_" + std::to_string(i));
        del_req.set_client_id("perf_client");

        auto start = std::chrono::high_resolution_clock::now();
        auto result = client_->Delete(del_req);
        auto end = std::chrono::high_resolution_clock::now();

        ASSERT_EQ(result.status, RPCStatus::SUCCESS);

        double latency_us = std::chrono::duration<double, std::micro>(end - start).count();
        latencies.push_back(latency_us);
    }

    Stats stats = CalculateStats(latencies);

    std::cout << "\n=== DELETE Latency Benchmark (" << num_iterations << " operations) ===" << std::endl;
    std::cout << "  Mean:   " << FormatLatency(stats.mean) << std::endl;
    std::cout << "  Median: " << FormatLatency(stats.median) << std::endl;
    std::cout << "  Min:    " << FormatLatency(stats.min) << std::endl;
    std::cout << "  Max:    " << FormatLatency(stats.max) << std::endl;
    std::cout << "  P95:    " << FormatLatency(stats.percentile_95) << std::endl;
    std::cout << "  P99:    " << FormatLatency(stats.percentile_99) << std::endl;

    DisconnectClient();
    StopServer();
}

TEST_F(PerformanceTest, MixedWorkloadBenchmark) {
    StartServer();
    ConnectClient();

    const int num_iterations = 1000;
    std::vector<double> put_latencies;
    std::vector<double> get_latencies;
    std::vector<double> delete_latencies;

    for (int i = 0; i < num_iterations; ++i) {
        // PUT operation
        PutRequest put_req;
        put_req.set_key("mixed_key_" + std::to_string(i));
        put_req.set_dev_ptr(0x1000 + i);
        put_req.set_size(1024);
        put_req.set_data_type(DataType::FP32);
        put_req.set_client_id("perf_client");

        auto start = std::chrono::high_resolution_clock::now();
        auto put_result = client_->Put(put_req);
        auto end = std::chrono::high_resolution_clock::now();

        ASSERT_EQ(put_result.status, RPCStatus::SUCCESS);
        put_latencies.push_back(std::chrono::duration<double, std::micro>(end - start).count());

        // GET operation
        GetRequest get_req;
        get_req.set_key("mixed_key_" + std::to_string(i));
        get_req.set_client_id("perf_client");

        start = std::chrono::high_resolution_clock::now();
        auto get_result = client_->Get(get_req);
        end = std::chrono::high_resolution_clock::now();

        ASSERT_EQ(get_result.status, RPCStatus::SUCCESS);
        get_latencies.push_back(std::chrono::duration<double, std::micro>(end - start).count());

        // DELETE every 10th key
        if (i % 10 == 0) {
            DeleteRequest del_req;
            del_req.set_key("mixed_key_" + std::to_string(i));
            del_req.set_client_id("perf_client");

            start = std::chrono::high_resolution_clock::now();
            auto del_result = client_->Delete(del_req);
            end = std::chrono::high_resolution_clock::now();

            ASSERT_EQ(del_result.status, RPCStatus::SUCCESS);
            delete_latencies.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        }
    }

    Stats put_stats = CalculateStats(put_latencies);
    Stats get_stats = CalculateStats(get_latencies);
    Stats del_stats = CalculateStats(delete_latencies);

    std::cout << "\n=== Mixed Workload Benchmark (" << num_iterations << " PUT/GET, "
              << delete_latencies.size() << " DELETE) ===" << std::endl;

    std::cout << "\n  PUT Statistics:" << std::endl;
    std::cout << "    Mean:   " << FormatLatency(put_stats.mean) << std::endl;
    std::cout << "    Median: " << FormatLatency(put_stats.median) << std::endl;
    std::cout << "    P95:    " << FormatLatency(put_stats.percentile_95) << std::endl;
    std::cout << "    P99:    " << FormatLatency(put_stats.percentile_99) << std::endl;

    std::cout << "\n  GET Statistics:" << std::endl;
    std::cout << "    Mean:   " << FormatLatency(get_stats.mean) << std::endl;
    std::cout << "    Median: " << FormatLatency(get_stats.median) << std::endl;
    std::cout << "    P95:    " << FormatLatency(get_stats.percentile_95) << std::endl;
    std::cout << "    P99:    " << FormatLatency(get_stats.percentile_99) << std::endl;

    std::cout << "\n  DELETE Statistics:" << std::endl;
    std::cout << "    Mean:   " << FormatLatency(del_stats.mean) << std::endl;
    std::cout << "    Median: " << FormatLatency(del_stats.median) << std::endl;
    std::cout << "    P95:    " << FormatLatency(del_stats.percentile_95) << std::endl;
    std::cout << "    P99:    " << FormatLatency(del_stats.percentile_99) << std::endl;

    DisconnectClient();
    StopServer();
}

//==============================================================================
// Throughput Benchmarks
//==============================================================================

TEST_F(PerformanceTest, ThroughputBenchmark) {
    StartServer();
    ConnectClient();

    const int num_operations = 10000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_operations; ++i) {
        PutRequest put_req;
        put_req.set_key("throughput_key_" + std::to_string(i));
        put_req.set_dev_ptr(0x1000 + i);
        put_req.set_size(1024);
        put_req.set_data_type(DataType::FP32);
        put_req.set_client_id("perf_client");

        auto result = client_->Put(put_req);
        ASSERT_EQ(result.status, RPCStatus::SUCCESS);
    }

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_seconds = std::chrono::duration<double>(end - start).count();
    double ops_per_second = num_operations / elapsed_seconds;

    std::cout << "\n=== Throughput Benchmark ===" << std::endl;
    std::cout << "  Total operations: " << num_operations << std::endl;
    std::cout << "  Elapsed time:     " << std::fixed << std::setprecision(2)
              << elapsed_seconds << " seconds" << std::endl;
    std::cout << "  Throughput:       " << std::fixed << std::setprecision(2)
              << ops_per_second << " ops/sec" << std::endl;

    DisconnectClient();
    StopServer();
}

//==============================================================================
// Connection Overhead Benchmark
//==============================================================================

TEST_F(PerformanceTest, ConnectionOverheadBenchmark) {
    StartServer();

    const int num_connections = 100;
    std::vector<double> connect_latencies;

    for (int i = 0; i < num_connections; ++i) {
        UCXClientConfig config;
        config.use_rdma = false;
        UCXControlClient client(config);
        ASSERT_TRUE(client.Initialize());

        auto start = std::chrono::high_resolution_clock::now();
        ASSERT_TRUE(client.Connect("127.0.0.1", test_port_));
        auto end = std::chrono::high_resolution_clock::now();

        double latency_us = std::chrono::duration<double, std::micro>(end - start).count();
        connect_latencies.push_back(latency_us);

        client.Disconnect();
    }

    Stats stats = CalculateStats(connect_latencies);

    std::cout << "\n=== Connection Overhead Benchmark (" << num_connections << " connections) ===" << std::endl;
    std::cout << "  Mean:   " << FormatLatency(stats.mean) << std::endl;
    std::cout << "  Median: " << FormatLatency(stats.median) << std::endl;
    std::cout << "  Min:    " << FormatLatency(stats.min) << std::endl;
    std::cout << "  Max:    " << FormatLatency(stats.max) << std::endl;
    std::cout << "  P95:    " << FormatLatency(stats.percentile_95) << std::endl;
    std::cout << "  P99:    " << FormatLatency(stats.percentile_99) << std::endl;

    StopServer();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
