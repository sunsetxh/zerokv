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

#ifndef ZEROKV_ZEROKV_CLIENT_H
#define ZEROKV_ZEROKV_CLIENT_H

#include <memory>
#include <string>

#include "zerokv/zerokv_types.h"
#include "zerokv/status.h"

namespace zerokv {

// Forward declarations
class UCXControlClient;
class PerformanceMonitor;
struct P2PComm;
struct HcclRootInfo;

/**
 * @brief ZeroKV Client
 *
 * Client for retrieving data from ZeroKV Server.
 * Supports zero-copy NPU-to-NPU memory transfer via P2P-Transfer.
 */
class ZeroKVClient {
public:
    /**
     * @brief Constructor
     * @param deviceId Local NPU device ID
     */
    explicit ZeroKVClient(uint32_t deviceId);

    /**
     * @brief Constructor with config
     * @param config Client configuration
     */
    explicit ZeroKVClient(const ClientConfig& config);

    /**
     * @brief Destructor
     */
    ~ZeroKVClient();

    /**
     * @brief Connect to server
     * @param serverIp Server IP address
     * @param port Server port
     * @return Status::OK() on success, error status otherwise
     */
    Status Connect(const std::string& serverIp, uint16_t port);

    /**
     * @brief Get data from server (synchronous)
     *
     * Retrieves data from server and transfers to local NPU memory.
     * This function blocks until transfer completes.
     *
     * @param key Key to retrieve
     * @param localDevPtr Local NPU memory pointer (pre-allocated)
     * @param size Expected data size in bytes
     * @return Status::OK() on success, error status otherwise
     */
    Status Get(const std::string& key,
               void* localDevPtr,
               size_t size);

    /**
     * @brief Get data from server (asynchronous)
     *
     * Retrieves data from server and transfers to local NPU memory asynchronously.
     * Returns immediately, transfer happens in background.
     * Use aclrtSynchronizeStream to wait for completion.
     *
     * @param key Key to retrieve
     * @param localDevPtr Local NPU memory pointer (pre-allocated)
     * @param size Expected data size in bytes
     * @param stream ACL stream for asynchronous execution
     * @return Status::OK() on success, error status otherwise
     */
    Status GetAsync(const std::string& key,
                   void* localDevPtr,
                   size_t size,
                   void* stream);  // aclrtStream

    /**
     * @brief Batch get multiple keys
     *
     * Retrieves multiple keys in a single request for better performance.
     *
     * @param keys Vector of keys to retrieve
     * @param localDevPtrs Vector of local NPU memory pointers (pre-allocated)
     * @param sizes Vector of expected data sizes
     * @return Status::OK() on success, error status otherwise
     */
    Status BatchGet(const std::vector<std::string>& keys,
                   const std::vector<void*>& localDevPtrs,
                   const std::vector<size_t>& sizes);

    /**
     * @brief Disconnect from server
     * @return Status::OK() on success, error status otherwise
     */
    Status Disconnect();

    /**
     * @brief Get performance monitor
     * @return Shared pointer to performance monitor
     */
    std::shared_ptr<PerformanceMonitor> GetMonitor();

    /**
     * @brief Check if connected to server
     * @return True if connected, false otherwise
     */
    bool IsConnected() const;

    /**
     * @brief Get client ID
     * @return Client unique identifier
     */
    const std::string& GetClientId() const;

private:
    // Disable copy and move
    ZeroKVClient(const ZeroKVClient&) = delete;
    ZeroKVClient& operator=(const ZeroKVClient&) = delete;

    // Helper functions
    Status SendGetRequest(const std::string& key,
                         std::string& serverRootInfo,
                         void*& serverDevPtr,
                         size_t& serverSize);

    Status EstablishP2PConnection(const std::string& serverRootInfo);

    Status ValidateDevicePointer(void* devPtr, size_t size);

    Status GetWithTimeout(const std::string& key,
                         void* localDevPtr,
                         size_t size,
                         uint32_t timeoutMs);

    std::string GenerateClientId();

    // Configuration
    ClientConfig config_;

    // UCX control client
    std::unique_ptr<UCXControlClient> ucxClient_;

    // P2P communication context
    std::shared_ptr<P2PComm> localP2PComm_;
    std::unique_ptr<HcclRootInfo> localRootInfo_;

    // Performance monitor
    std::shared_ptr<PerformanceMonitor> monitor_;

    // Connection state
    std::atomic<bool> connected_{false};
    std::string serverIp_;
    uint16_t serverPort_;
};

}  // namespace zerokv

#endif  // ZEROKV_ZEROKV_CLIENT_H
