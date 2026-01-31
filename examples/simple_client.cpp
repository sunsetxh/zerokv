/**
 * Simple ZeroKV Client Example
 *
 * This example demonstrates how to:
 * 1. Connect to an ZeroKV Server
 * 2. Retrieve data from the server
 * 3. Verify data correctness
 */

#include <iostream>
#include <vector>
#include <chrono>

#include "zerokv/zerokv_client.h"

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
#define ACL_MEMCPY_DEVICE_TO_HOST 1
#define aclInit(config) 0
#define aclFinalize() 0
#define aclrtSetDevice(id) 0
#endif

int main(int argc, char** argv) {
    // Parse command line
    std::string serverIp = "127.0.0.1";
    uint16_t port = 50051;
    uint32_t deviceId = 1;

    if (argc >= 2) {
        serverIp = argv[1];
    }
    if (argc >= 3) {
        port = std::atoi(argv[2]);
    }
    if (argc >= 4) {
        deviceId = std::atoi(argv[3]);
    }

    std::cout << "ZeroKV Client Example\n";
    std::cout << "Server: " << serverIp << ":" << port << "\n";
    std::cout << "Device ID: " << deviceId << "\n\n";

    // Initialize ACL
    aclInit(nullptr);
    aclrtSetDevice(deviceId);

    // Create and connect client
    zerokv::ZeroKVClient client(deviceId);
    auto status = client.Connect(serverIp, port);
    if (!status.ok()) {
        std::cerr << "Failed to connect: " << status.message() << "\n";
        return 1;
    }

    std::cout << "Connected to server successfully\n";
    std::cout << "Client ID: " << client.GetClientId() << "\n\n";

    // Allocate local NPU memory
    const size_t dataSize = 1024 * 1024 * sizeof(float);  // 1M floats = 4MB
    void* localDevPtr;
    aclrtMalloc(&localDevPtr, dataSize, 0);

    // Get data from server
    std::cout << "Retrieving 'model_weights' from server...\n";
    auto startTime = std::chrono::high_resolution_clock::now();

    status = client.Get("model_weights", localDevPtr, dataSize);

    auto endTime = std::chrono::high_resolution_clock::now();
    auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count();

    if (!status.ok()) {
        std::cerr << "Failed to get data: " << status.message() << "\n";
        return 1;
    }

    std::cout << "Data retrieved successfully\n";
    std::cout << "Transfer time: " << durationUs << " us\n";
    std::cout << "Transfer size: " << dataSize / (1024.0 * 1024.0) << " MB\n";
    std::cout << "Bandwidth: " << (dataSize / (1024.0 * 1024.0)) / (durationUs / 1e6) << " MB/s\n\n";

    // Verify data (copy to host and check)
    std::vector<float> hostData(1024 * 1024);
    aclrtMemcpy(hostData.data(), dataSize, localDevPtr, dataSize, ACL_MEMCPY_DEVICE_TO_HOST);

    // Check if data is correct (should be all 1.0f from server example)
    bool dataCorrect = true;
    for (size_t i = 0; i < hostData.size(); ++i) {
        if (std::abs(hostData[i] - 1.0f) > 1e-6) {
            dataCorrect = false;
            std::cerr << "Data mismatch at index " << i << ": " << hostData[i] << " != 1.0\n";
            break;
        }
    }

    if (dataCorrect) {
        std::cout << "Data verification: PASSED\n\n";
    } else {
        std::cout << "Data verification: FAILED\n\n";
    }

    // Get performance statistics
    auto monitor = client.GetMonitor();
    auto getStats = monitor->GetStats(zerokv::OperationType::GET);

    std::cout << "Performance Statistics:\n";
    std::cout << "  Total GET Ops: " << getStats.totalOps << "\n";
    std::cout << "  Success Rate: " << (100.0 * getStats.successOps / getStats.totalOps) << "%\n";
    std::cout << "  Avg Latency: " << getStats.avgLatencyUs << " us\n";
    std::cout << "  P50 Latency: " << getStats.p50LatencyUs << " us\n";
    std::cout << "  P95 Latency: " << getStats.p95LatencyUs << " us\n";
    std::cout << "  P99 Latency: " << getStats.p99LatencyUs << " us\n";
    std::cout << "  Throughput: " << getStats.throughputMBps << " MB/s\n\n";

    // Perform multiple Get operations to test performance
    std::cout << "Running 10 Get operations...\n";
    for (int i = 0; i < 10; ++i) {
        status = client.Get("model_weights", localDevPtr, dataSize);
        if (!status.ok()) {
            std::cerr << "Get #" << i << " failed: " << status.message() << "\n";
        } else {
            std::cout << "  Get #" << i << " completed\n";
        }
    }

    // Print updated statistics
    getStats = monitor->GetStats(zerokv::OperationType::GET);
    std::cout << "\nUpdated Performance Statistics:\n";
    std::cout << "  Total GET Ops: " << getStats.totalOps << "\n";
    std::cout << "  P95 Latency: " << getStats.p95LatencyUs << " us\n";
    std::cout << "  Throughput: " << getStats.throughputMBps << " MB/s\n";

    // Cleanup
    client.Disconnect();
    aclrtFree(localDevPtr);
    aclFinalize();

    std::cout << "\nClient disconnected\n";

    return 0;
}
