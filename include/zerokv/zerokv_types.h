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

#ifndef ZEROKV_ZEROKV_TYPES_H
#define ZEROKV_ZEROKV_TYPES_H

#include <cstdint>
#include <string>

namespace zerokv {

/**
 * @brief Operation types for performance monitoring
 */
enum class OperationType {
    PUT,        // Register NPU memory to KV store
    GET,        // Retrieve data from KV store
    DELETE,     // Delete key from KV store
    P2P_SEND,   // P2P send operation
    P2P_RECV    // P2P receive operation
};

/**
 * @brief Data types supported by ZeroKV
 * Maps to HcclDataType
 */
enum class DataType {
    FP32,   // 32-bit floating point
    FP16,   // 16-bit floating point
    INT32,  // 32-bit integer
    INT64,  // 64-bit integer
    UINT8   // 8-bit unsigned integer
};

/**
 * @brief Metadata for a key-value pair
 */
struct KVMetadata {
    void* devPtr;               // NPU device memory pointer
    uint64_t size;              // Data size in bytes
    uint32_t deviceId;          // NPU device ID
    DataType dataType;          // Data type
    std::string ownerId;        // Client ID that registered this KV
    uint64_t timestamp;         // Registration timestamp (microseconds)

    KVMetadata()
        : devPtr(nullptr), size(0), deviceId(0),
          dataType(DataType::FP32), timestamp(0) {}

    KVMetadata(void* ptr, uint64_t sz, uint32_t devId, DataType dtype,
               const std::string& owner, uint64_t ts)
        : devPtr(ptr), size(sz), deviceId(devId),
          dataType(dtype), ownerId(owner), timestamp(ts) {}
};

/**
 * @brief Performance metrics for a single operation
 */
struct OperationMetrics {
    OperationType type;         // Operation type
    uint64_t latencyUs;         // Latency in microseconds
    uint64_t dataSize;          // Data size in bytes
    uint64_t timestamp;         // Timestamp in microseconds
    bool success;               // Whether operation succeeded

    OperationMetrics()
        : type(OperationType::GET), latencyUs(0), dataSize(0),
          timestamp(0), success(false) {}

    OperationMetrics(OperationType t, uint64_t lat, uint64_t sz,
                    uint64_t ts, bool succ)
        : type(t), latencyUs(lat), dataSize(sz),
          timestamp(ts), success(succ) {}
};

/**
 * @brief Aggregated statistics for an operation type
 */
struct AggregatedStats {
    uint64_t totalOps;          // Total number of operations
    uint64_t successOps;        // Number of successful operations
    uint64_t failedOps;         // Number of failed operations
    double avgLatencyUs;        // Average latency in microseconds
    double p50LatencyUs;        // P50 latency in microseconds
    double p95LatencyUs;        // P95 latency in microseconds
    double p99LatencyUs;        // P99 latency in microseconds
    double throughputMBps;      // Throughput in MB/s

    AggregatedStats()
        : totalOps(0), successOps(0), failedOps(0),
          avgLatencyUs(0.0), p50LatencyUs(0.0),
          p95LatencyUs(0.0), p99LatencyUs(0.0),
          throughputMBps(0.0) {}
};

/**
 * @brief Server statistics
 */
struct ServerStats {
    uint64_t kvCount;           // Number of KV pairs
    uint64_t totalMemoryBytes;  // Total registered memory in bytes
    uint32_t activeConnections; // Number of active client connections
    uint64_t uptimeSeconds;     // Server uptime in seconds

    ServerStats()
        : kvCount(0), totalMemoryBytes(0),
          activeConnections(0), uptimeSeconds(0) {}
};

/**
 * @brief Configuration for ZeroKV Server
 */
struct ServerConfig {
    uint32_t deviceId;          // NPU device ID
    std::string ip;             // Listen IP address
    uint16_t port;              // Listen port
    uint16_t monitorPort;       // Prometheus metrics port
    uint32_t maxConnections;    // Maximum number of client connections
    uint32_t workerThreads;     // Number of worker threads

    ServerConfig()
        : deviceId(0), ip("0.0.0.0"), port(50051),
          monitorPort(9090), maxConnections(1000), workerThreads(4) {}
};

/**
 * @brief Configuration for ZeroKV Client
 */
struct ClientConfig {
    uint32_t deviceId;          // Local NPU device ID
    std::string clientId;       // Unique client identifier
    uint32_t timeoutMs;         // Request timeout in milliseconds
    uint32_t retryCount;        // Number of retries on failure

    ClientConfig()
        : deviceId(0), timeoutMs(5000), retryCount(3) {}
};

/**
 * @brief Convert OperationType to string
 */
inline const char* OperationTypeToString(OperationType type) {
    switch (type) {
        case OperationType::PUT:       return "PUT";
        case OperationType::GET:       return "GET";
        case OperationType::DELETE:    return "DELETE";
        case OperationType::P2P_SEND:  return "P2P_SEND";
        case OperationType::P2P_RECV:  return "P2P_RECV";
        default:                       return "UNKNOWN";
    }
}

/**
 * @brief Convert DataType to string
 */
inline const char* DataTypeToString(DataType type) {
    switch (type) {
        case DataType::FP32:   return "FP32";
        case DataType::FP16:   return "FP16";
        case DataType::INT32:  return "INT32";
        case DataType::INT64:  return "INT64";
        case DataType::UINT8:  return "UINT8";
        default:               return "UNKNOWN";
    }
}

/**
 * @brief Get element size in bytes for a DataType
 */
inline size_t GetDataTypeSize(DataType type) {
    switch (type) {
        case DataType::FP32:   return 4;
        case DataType::FP16:   return 2;
        case DataType::INT32:  return 4;
        case DataType::INT64:  return 8;
        case DataType::UINT8:  return 1;
        default:               return 0;
    }
}

}  // namespace zerokv

#endif  // ZEROKV_ZEROKV_TYPES_H
