/**
 * Simple ZeroKV Server Example
 *
 * This example demonstrates how to:
 * 1. Start an ZeroKV Server
 * 2. Register NPU memory to KV store
 * 3. Monitor performance
 */

#include <iostream>
#include <vector>
#include <chrono>
#include <thread>

#include "zerokv/zerokv_server.h"

// Mock ACL functions (for environments without real NPU)
#ifndef USE_REAL_NPU
void* MockMalloc(size_t size) {
    return malloc(size);
}

void MockFree(void* ptr) {
    free(ptr);
}

void MockMemcpy(void* dst, size_t dstSize, const void* src, size_t srcSize, int kind) {
    memcpy(dst, src, std::min(dstSize, srcSize));
}

#define aclrtMalloc(ptr, size, policy) (*(ptr) = MockMalloc(size), 0)
#define aclrtFree(ptr) (MockFree(ptr), 0)
#define aclrtMemcpy(dst, dstSize, src, srcSize, kind) (MockMemcpy(dst, dstSize, src, srcSize, kind), 0)
#define ACL_MEMCPY_HOST_TO_DEVICE 0
#define aclInit(config) 0
#define aclFinalize() 0
#define aclrtSetDevice(id) 0
#endif

int main(int argc, char** argv) {
    // Parse command line
    uint32_t deviceId = 0;
    std::string ip = "0.0.0.0";
    uint16_t port = 50051;

    if (argc >= 2) {
        deviceId = std::atoi(argv[1]);
    }
    if (argc >= 3) {
        ip = argv[2];
    }
    if (argc >= 4) {
        port = std::atoi(argv[3]);
    }

    std::cout << "ZeroKV Server Example\n";
    std::cout << "Device ID: " << deviceId << "\n";
    std::cout << "Listen on: " << ip << ":" << port << "\n\n";

    // Initialize ACL
    aclInit(nullptr);
    aclrtSetDevice(deviceId);

    // Create and start server
    zerokv::ZeroKVServer server(deviceId);
    auto status = server.Start(ip, port);
    if (!status.ok()) {
        std::cerr << "Failed to start server: " << status.message() << "\n";
        return 1;
    }

    std::cout << "Server started successfully\n";

    // Allocate some NPU memory and register to KV store
    const size_t dataSize = 1024 * 1024 * sizeof(float);  // 1M floats = 4MB
    void* devPtr1;
    void* devPtr2;

    aclrtMalloc(&devPtr1, dataSize, 0);
    aclrtMalloc(&devPtr2, dataSize * 2, 0);

    // Fill with sample data
    std::vector<float> hostData1(1024 * 1024, 1.0f);
    std::vector<float> hostData2(1024 * 1024 * 2, 2.0f);

    aclrtMemcpy(devPtr1, dataSize, hostData1.data(), dataSize, ACL_MEMCPY_HOST_TO_DEVICE);
    aclrtMemcpy(devPtr2, dataSize * 2, hostData2.data(), dataSize * 2, ACL_MEMCPY_HOST_TO_DEVICE);

    // Register to KV store
    status = server.Put("model_weights", devPtr1, dataSize, zerokv::DataType::FP32);
    if (!status.ok()) {
        std::cerr << "Failed to put model_weights: " << status.message() << "\n";
        return 1;
    }
    std::cout << "Registered 'model_weights' (4MB)\n";

    status = server.Put("gradients", devPtr2, dataSize * 2, zerokv::DataType::FP32);
    if (!status.ok()) {
        std::cerr << "Failed to put gradients: " << status.message() << "\n";
        return 1;
    }
    std::cout << "Registered 'gradients' (8MB)\n";

    // Get server statistics
    auto stats = server.GetStats();
    std::cout << "\nServer Statistics:\n";
    std::cout << "  KV Count: " << stats.kvCount << "\n";
    std::cout << "  Total Memory: " << stats.totalMemoryBytes / (1024.0 * 1024.0) << " MB\n";
    std::cout << "  Active Connections: " << stats.activeConnections << "\n";

    // Start performance monitoring
    auto monitor = server.GetMonitor();
    std::cout << "\nStarting real-time performance monitor...\n";
    std::cout << "Press Ctrl+C to stop\n\n";
    monitor->StartRealTimeDisplay();

    // Keep server running
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Update stats every 5 seconds
        static int counter = 0;
        if (++counter % 5 == 0) {
            stats = server.GetStats();
            auto putStats = monitor->GetStats(zerokv::OperationType::PUT);
            auto getStats = monitor->GetStats(zerokv::OperationType::GET);

            std::cout << "\n--- Server Status ---\n";
            std::cout << "KV Count: " << stats.kvCount << "\n";
            std::cout << "Active Connections: " << stats.activeConnections << "\n";
            std::cout << "PUT Total Ops: " << putStats.totalOps << "\n";
            std::cout << "GET Total Ops: " << getStats.totalOps << "\n";
            if (getStats.totalOps > 0) {
                std::cout << "GET P95 Latency: " << getStats.p95LatencyUs << " us\n";
                std::cout << "GET Throughput: " << getStats.throughputMBps << " MB/s\n";
            }
            std::cout << "--------------------\n\n";
        }
    }

    // Cleanup (unreachable in this example)
    monitor->StopRealTimeDisplay();
    server.Shutdown();
    aclrtFree(devPtr1);
    aclrtFree(devPtr2);
    aclFinalize();

    return 0;
}
