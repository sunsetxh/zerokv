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
 * @file ucx_control_server.h
 * @brief UCX-based control server for ZeroKV
 *
 * This file implements the control plane server using UCX for communication.
 * It handles client connections, RPC requests (Put/Get/Delete), and manages
 * the in-memory KV store.
 */

#ifndef ZEROKV_COMMON_UCX_CONTROL_SERVER_H
#define ZEROKV_COMMON_UCX_CONTROL_SERVER_H

#ifdef USE_UCX_STUB
#include "common/ucx_stub.h"
#else
#include <ucp/api/ucp.h>
#endif

#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <functional>
#include <chrono>

#include "zerokv.pb.h"

namespace zerokv {

// Import protobuf types
using proto::PutRequest;
using proto::PutResponse;
using proto::GetRequest;
using proto::GetResponse;
using proto::DeleteRequest;
using proto::DeleteResponse;
using proto::ServerStatsRequest;
using proto::ServerStatsResponse;
using proto::DataType;

// Server configuration
struct UCXServerConfig {
    std::string listen_address = "0.0.0.0";  // Listen on all interfaces
    uint16_t listen_port = 18515;             // Default port
    bool use_rdma = true;                     // Use RDMA if available
    size_t max_connections = 256;             // Max concurrent clients
    size_t max_kv_size = 10UL * 1024 * 1024 * 1024;  // 10GB max KV storage
    uint32_t worker_progress_timeout_ms = 10; // Progress timeout

    // Memory management
    size_t allocation_alignment = 512;        // NPU memory alignment
};

// KV entry
struct KVEntry {
    uint64_t dev_ptr = 0;
    uint64_t size = 0;
    DataType data_type = DataType::FP32;
    std::string client_id;
    std::chrono::system_clock::time_point timestamp;
};

// Client connection info
struct ClientConnection {
    ucp_ep_h endpoint = nullptr;
    std::string client_id;
    std::string remote_address;
    std::chrono::system_clock::time_point connect_time;
};

// RPC handler callback type
using MessageHandler = std::function<std::string(const std::string& request_data)>;

/**
 * @brief UCX Control Server for ZeroKV
 *
 * Provides control plane communication using UCX for:
 * - Client connection management
 * - KV operations (Put/Get/Delete)
 * - Statistics and monitoring
 */
class UCXControlServer {
    // Friend function for UCX callback
    friend void connection_request_callback(ucp_conn_request_h conn_request, void* arg);

public:
    explicit UCXControlServer(const UCXServerConfig& config = UCXServerConfig{});
    ~UCXControlServer();

    // Prevent copying
    UCXControlServer(const UCXControlServer&) = delete;
    UCXControlServer& operator=(const UCXControlServer&) = delete;

    /**
     * @brief Initialize the server (create context, worker, listener)
     * @return true on success, false on failure
     */
    bool Initialize();

    /**
     * @brief Start the server (begin accepting connections)
     * @return true on success, false on failure
     */
    bool Start();

    /**
     * @brief Stop the server and cleanup resources
     */
    void Stop();

    /**
     * @brief Run the server event loop (blocking)
     * @param stop_after_ms Timeout in milliseconds (0 = run forever)
     * @return true if stopped normally, false on error
     */
    bool Run(uint32_t stop_after_ms = 0);

    /**
     * @brief Get the server's listener address (for clients to connect)
     * @return String representation of the address
     */
    std::string GetListenAddress() const;

    /**
     * @brief Get server statistics
     */
    ServerStatsResponse GetStats() const;

    /**
     * @brief Check if server is running
     */
    bool IsRunning() const { return running_; }

    /**
     * @brief Get number of active connections
     */
    size_t GetConnectionCount() const {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        return client_connections_.size();
    }

    /**
     * @brief Get number of KV entries
     */
    size_t GetKVCount() const {
        std::lock_guard<std::mutex> lock(kv_store_mutex_);
        return kv_store_.size();
    }

private:
    // Initialization helpers
    bool CreateUCPContext();
    bool CreateUCPWorker();
    bool CreateListener();

    // Connection management
    void HandleConnectionRequest(ucp_conn_request_h conn_request, void* arg);
    void AcceptConnection(ucp_conn_request_h conn_request);
    void CloseConnection(const std::string& client_id);

    // Message handlers
    std::string HandlePutRequest(const PutRequest& request);
    std::string HandleGetRequest(const GetRequest& request);
    std::string HandleDeleteRequest(const DeleteRequest& request);
    std::string HandleStatsRequest(const ServerStatsRequest& request);

    // Send response to client
    bool SendResponse(ucp_ep_h endpoint, const std::string& response_data);

    // Worker progress
    void ProgressWorker();

    // UCX objects
    ucp_context_h ucp_context_ = nullptr;
    ucp_worker_h ucp_worker_ = nullptr;
    ucp_listener_h ucp_listener_ = nullptr;

    // Configuration
    UCXServerConfig config_;

    // State
    bool running_ = false;
    bool initialized_ = false;
    std::string listen_address_;

    // KV store
    std::unordered_map<std::string, KVEntry> kv_store_;
    mutable std::mutex kv_store_mutex_;

    // Client connections
    std::unordered_map<std::string, ClientConnection> client_connections_;
    mutable std::mutex connections_mutex_;

    // Statistics
    std::chrono::system_clock::time_point start_time_;
    uint64_t total_memory_used_ = 0;

    // Constants
    static constexpr size_t MAX_MESSAGE_SIZE = 4 * 1024 * 1024;  // 4MB max message
};

}  // namespace zerokv

#endif  // ZEROKV_COMMON_UCX_CONTROL_SERVER_H
