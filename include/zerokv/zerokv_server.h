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

#ifndef ZEROKV_ZEROKV_SERVER_H
#define ZEROKV_ZEROKV_SERVER_H

#include <memory>
#include <string>
#include <unordered_map>
#include <shared_mutex>

#include "zerokv/zerokv_types.h"
#include "zerokv/status.h"

namespace zerokv {

// Forward declarations
class UCXControlServer;
class PerformanceMonitor;
struct P2PComm;

/**
 * @brief ZeroKV Server
 *
 * Manages NPU memory KV storage and handles client requests.
 * Supports zero-copy NPU-to-NPU memory transfer via P2P-Transfer.
 */
class ZeroKVServer {
public:
    /**
     * @brief Constructor
     * @param deviceId NPU device ID to use
     */
    explicit ZeroKVServer(uint32_t deviceId);

    /**
     * @brief Constructor with config
     * @param config Server configuration
     */
    explicit ZeroKVServer(const ServerConfig& config);

    /**
     * @brief Destructor
     */
    ~ZeroKVServer();

    /**
     * @brief Start the server
     * @param ip IP address to listen on ("0.0.0.0" for all interfaces)
     * @param port Port to listen on
     * @return Status::OK() on success, error status otherwise
     */
    Status Start(const std::string& ip, uint16_t port);

    /**
     * @brief Register NPU memory to KV store
     *
     * This function registers an existing NPU memory region without copying data.
     * The memory must remain valid until deleted from the store.
     *
     * @param key Unique key identifier
     * @param devPtr NPU device memory pointer (allocated via aclrtMalloc)
     * @param size Data size in bytes
     * @param dataType Data type (default: FP32)
     * @return Status::OK() on success, error status otherwise
     */
    Status Put(const std::string& key,
               void* devPtr,
               size_t size,
               DataType dataType = DataType::FP32);

    /**
     * @brief Delete a key from KV store
     *
     * Note: This only removes metadata, does not free NPU memory.
     *
     * @param key Key to delete
     * @return Status::OK() on success, error status otherwise
     */
    Status Delete(const std::string& key);

    /**
     * @brief Get server statistics
     * @return Server statistics
     */
    ServerStats GetStats() const;

    /**
     * @brief Get performance monitor
     * @return Shared pointer to performance monitor
     */
    std::shared_ptr<PerformanceMonitor> GetMonitor();

    /**
     * @brief Shutdown the server
     *
     * Gracefully shutdown the server:
     * - Stop accepting new connections
     * - Wait for pending transfers to complete
     * - Notify all clients
     * - Release resources
     *
     * @return Status::OK() on success, error status otherwise
     */
    Status Shutdown();

    /**
     * @brief Check if server is running
     * @return True if running, false otherwise
     */
    bool IsRunning() const;

private:
    // Disable copy and move
    ZeroKVServer(const ZeroKVServer&) = delete;
    ZeroKVServer& operator=(const ZeroKVServer&) = delete;

    // RPC handlers
    void HandlePutRequest(const std::string& clientId,
                         const std::string& key,
                         void* devPtr,
                         size_t size,
                         DataType dataType);

    void HandleGetRequest(const std::string& clientId,
                         const std::string& key,
                         std::string& response);

    void HandleDeleteRequest(const std::string& clientId,
                            const std::string& key);

    // P2P connection management
    Status GetOrCreateP2PConnection(const std::string& clientId,
                                   const std::string& clientRootInfo);

    // Helper functions
    Status ValidateDevicePointer(void* devPtr, size_t size);
    Status ValidateKey(const std::string& key);
    uint64_t GetCurrentTimestamp();

    // Configuration
    ServerConfig config_;

    // KV metadata table
    std::unordered_map<std::string, KVMetadata> kvTable_;
    mutable std::shared_mutex kvTableMutex_;

    // P2P connections to clients
    std::unordered_map<std::string, std::shared_ptr<P2PComm>> clientP2PConns_;
    std::mutex connMutex_;

    // UCX control server
    std::unique_ptr<UCXControlServer> ucxServer_;

    // Performance monitor
    std::shared_ptr<PerformanceMonitor> monitor_;

    // Server state
    std::atomic<bool> running_{false};
    uint64_t startTime_;
};

}  // namespace zerokv

#endif  // ZEROKV_ZEROKV_SERVER_H
