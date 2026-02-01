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
 * @file ucx_control_client.h
 * @brief UCX-based control client for ZeroKV
 *
 * This file implements the control plane client using UCX for communication.
 * It handles server connections, RPC requests (Put/Get/Delete), and manages
 * request/response lifecycle.
 */

#ifndef ZEROKV_UCX_CONTROL_CLIENT_H
#define ZEROKV_UCX_CONTROL_CLIENT_H

#ifdef USE_UCX_STUB
#include "common/ucx_stub.h"
#else
#include <ucp/api/ucp.h>
#endif

#include <string>
#include <memory>
#include <mutex>
#include <condition_variable>
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

// Client configuration
struct UCXClientConfig {
    bool use_rdma = true;                     // Use RDMA if available
    uint32_t connect_timeout_ms = 5000;       // Connection timeout (5s)
    uint32_t request_timeout_ms = 10000;      // Request timeout (10s)
    uint32_t max_retries = 3;                 // Max retry attempts
    uint32_t retry_delay_ms = 100;            // Delay between retries
    uint32_t worker_progress_timeout_ms = 10; // Progress timeout
};

// RPC call result
enum class RPCStatus {
    SUCCESS = 0,
    TIMEOUT,
    NETWORK_ERROR,
    SERVER_ERROR,
    INVALID_RESPONSE,
    NOT_CONNECTED,
    RETRY_EXHAUSTED
};

// RPC result wrapper
template<typename T>
struct RPCResult {
    RPCStatus status;
    T response;
    std::string error_message;

    bool IsSuccess() const { return status == RPCStatus::SUCCESS; }
};

/**
 * @brief UCX Control Client for ZeroKV
 *
 * Provides control plane communication using UCX for:
 * - Server connection management
 * - RPC calls (Put/Get/Delete/Stats)
 * - Timeout and retry handling
 */
class UCXControlClient {
public:
    explicit UCXControlClient(const UCXClientConfig& config = UCXClientConfig{});
    ~UCXControlClient();

    // Prevent copying
    UCXControlClient(const UCXControlClient&) = delete;
    UCXControlClient& operator=(const UCXControlClient&) = delete;

    /**
     * @brief Initialize the client (create context, worker)
     * @return true on success, false on failure
     */
    bool Initialize();

    /**
     * @brief Connect to the server
     * @param server_address Server IP address or hostname
     * @param server_port Server port number
     * @return true on success, false on failure
     */
    bool Connect(const std::string& server_address, uint16_t server_port);

    /**
     * @brief Disconnect from the server
     */
    void Disconnect();

    /**
     * @brief Check if connected to server
     */
    bool IsConnected() const { return connected_; }

    /**
     * @brief Send Put request to server
     * @param request The Put request message
     * @return RPC result containing PutResponse
     */
    RPCResult<PutResponse> Put(const PutRequest& request);

    /**
     * @brief Send Get request to server
     * @param request The Get request message
     * @return RPC result containing GetResponse
     */
    RPCResult<GetResponse> Get(const GetRequest& request);

    /**
     * @brief Send Delete request to server
     * @param request The Delete request message
     * @return RPC result containing DeleteResponse
     */
    RPCResult<DeleteResponse> Delete(const DeleteRequest& request);

    /**
     * @brief Send Stats request to server
     * @param request The Stats request message
     * @return RPC result containing ServerStatsResponse
     */
    RPCResult<ServerStatsResponse> GetStats(const ServerStatsRequest& request);

    /**
     * @brief Get the server address
     */
    std::string GetServerAddress() const { return server_address_; }

    /**
     * @brief Get the server port
     */
    uint16_t GetServerPort() const { return server_port_; }

private:
    // Initialization helpers
    bool CreateUCPContext();
    bool CreateUCPWorker();
    bool CreateEndpoint(const std::string& server_address, uint16_t server_port);

    // RPC implementation
    template<typename RequestT, typename ResponseT>
    RPCResult<ResponseT> SendRequest(const RequestT& request,
                                    const std::string& request_type);

    // Send raw message and receive response
    bool SendMessage(const std::string& message);
    bool ReceiveResponse(std::string& response, uint32_t timeout_ms);

    // Worker progress
    void ProgressWorker();

    // Retry logic
    template<typename Func>
    bool RetryOperation(Func operation, uint32_t max_retries);

    // UCX objects
    ucp_context_h ucp_context_ = nullptr;
    ucp_worker_h ucp_worker_ = nullptr;
    ucp_ep_h ucp_endpoint_ = nullptr;

    // Configuration
    UCXClientConfig config_;

    // Connection state
    bool initialized_ = false;
    bool connected_ = false;
    std::string server_address_;
    uint16_t server_port_ = 0;

    // Thread safety
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    // Response handling
    std::string pending_response_;
    bool response_received_ = false;

    // Constants
    static constexpr size_t MAX_MESSAGE_SIZE = 4 * 1024 * 1024;  // 4MB max message
};

}  // namespace zerokv

#endif  // ZEROKV_UCX_CONTROL_CLIENT_H
