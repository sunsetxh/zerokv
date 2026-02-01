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
 * @file ucx_control_server.cpp
 * @brief Implementation of UCX-based control server for ZeroKV
 */

#include "zerokv/ucx_control_server.h"
#include <zerokv/logger.h>

#include <cstring>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace zerokv {

// UCX connection request callback (friend function, not static)
void connection_request_callback(ucp_conn_request_h conn_request, void* arg) {
    auto* server = static_cast<UCXControlServer*>(arg);
    server->HandleConnectionRequest(conn_request, arg);
}

//==============================================================================
// Constructor & Destructor
//==============================================================================

UCXControlServer::UCXControlServer(const UCXServerConfig& config)
    : config_(config), start_time_(std::chrono::system_clock::now()) {
}

UCXControlServer::~UCXControlServer() {
    Stop();
}

//==============================================================================
// Public Methods
//==============================================================================

bool UCXControlServer::Initialize() {
    if (initialized_) {
        LOG_ERROR( "Server already initialized");
        return false;
    }

    // Create UCP context
    if (!CreateUCPContext()) {
        LOG_ERROR( "Failed to create UCP context");
        return false;
    }

    // Create UCP worker
    if (!CreateUCPWorker()) {
        LOG_ERROR( "Failed to create UCP worker");
        return false;
    }

    // Create listener
    if (!CreateListener()) {
        LOG_ERROR( "Failed to create listener");
        return false;
    }

    initialized_ = true;
    LOG_INFO( "UCX Control Server initialized successfully");
    LOG_INFO( "Listen address: " << GetListenAddress());

    return true;
}

bool UCXControlServer::Start() {
    if (!initialized_) {
        LOG_ERROR( "Server not initialized");
        return false;
    }

    running_ = true;
    start_time_ = std::chrono::system_clock::now();
    LOG_INFO( "UCX Control Server started");

    return true;
}

void UCXControlServer::Stop() {
    if (!running_ && !initialized_) {
        return;  // Nothing to clean up
    }

    running_ = false;

    // Close all client connections
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto& [client_id, conn] : client_connections_) {
            if (conn.endpoint != nullptr) {
                ucp_request_param_t close_param;
                std::memset(&close_param, 0, sizeof(close_param));
                close_param.op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS;
                close_param.flags = UCP_EP_CLOSE_FLAG_FORCE;

                ucs_status_ptr_t request = ucp_ep_close_nbx(conn.endpoint, &close_param);
                if (request != nullptr) {
                    if (!UCS_PTR_IS_ERR(request)) {
                        // Wait for close to complete
                        ucs_status_t status;
                        do {
                            ucp_worker_progress(ucp_worker_);
                            status = ucp_request_check_status(request);
                        } while (status == UCS_INPROGRESS);
                        ucp_request_free(request);
                    }
                }
            }
        }
        client_connections_.clear();
    }

    // Destroy listener
    if (ucp_listener_ != nullptr) {
        ucp_listener_destroy(ucp_listener_);
        ucp_listener_ = nullptr;
    }

    // Destroy worker
    if (ucp_worker_ != nullptr) {
        ucp_worker_destroy(ucp_worker_);
        ucp_worker_ = nullptr;
    }

    // Destroy context
    if (ucp_context_ != nullptr) {
        ucp_cleanup(ucp_context_);
        ucp_context_ = nullptr;
    }

    initialized_ = false;

    initialized_ = false;
    LOG_INFO( "UCX Control Server stopped");
}

bool UCXControlServer::Run(uint32_t stop_after_ms) {
    if (!running_) {
        LOG_ERROR("Server not running");
        return false;
    }

    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::milliseconds(stop_after_ms);

    LOG_INFO("Server event loop started");

    while (running_) {
        // Progress UCX worker to handle communication
        ProgressWorker();

        // Check timeout
        if (stop_after_ms > 0) {
            auto now = std::chrono::steady_clock::now();
            if (now >= end_time) {
                LOG_INFO( "Server run timeout reached");
                break;
            }
        }

        // Small sleep to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    LOG_INFO( "Server event loop ended");
    return true;
}

std::string UCXControlServer::GetListenAddress() const {
    return listen_address_;
}

ServerStatsResponse UCXControlServer::GetStats() const {
    ServerStatsResponse stats;

    std::lock_guard<std::mutex> kv_lock(kv_store_mutex_);
    std::lock_guard<std::mutex> conn_lock(connections_mutex_);

    stats.set_kv_count(kv_store_.size());
    stats.set_total_memory_bytes(total_memory_used_);
    stats.set_active_connections(client_connections_.size());

    auto now = std::chrono::system_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_);
    stats.set_uptime_seconds(uptime.count());

    return stats;
}

//==============================================================================
// Initialization Helpers
//==============================================================================

bool UCXControlServer::CreateUCPContext() {
    // UCP parameters
    ucp_params_t ucp_params;
    std::memset(&ucp_params, 0, sizeof(ucp_params));
    ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES;
    ucp_params.features = UCP_FEATURE_STREAM | UCP_FEATURE_AM;

    // UCP config
    ucp_config_t* config = nullptr;
    ucs_status_t status = ucp_config_read(nullptr, nullptr, &config);
    if (status != UCS_OK) {
        LOG_ERROR( "Failed to read UCP config");
        return false;
    }

    // Modify config for RDMA/TCP
    if (config_.use_rdma) {
        ucp_config_modify(config, "TLS", "rc_x,sm,self");  // Prefer RDMA
    } else {
        ucp_config_modify(config, "TLS", "tcp,sm,self");  // Use TCP
    }

    // Initialize UCP context
    status = ucp_init(&ucp_params, config, &ucp_context_);
    ucp_config_release(config);

    if (status != UCS_OK) {
        LOG_ERROR( "Failed to initialize UCP context: "
                  << ucs_status_string(status));
        return false;
    }

    return true;
}

bool UCXControlServer::CreateUCPWorker() {
    // Worker parameters
    ucp_worker_params_t worker_params;
    std::memset(&worker_params, 0, sizeof(worker_params));
    worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_SERIALIZED;

    // Create worker
    ucs_status_t status = ucp_worker_create(ucp_context_, &worker_params, &ucp_worker_);
    if (status != UCS_OK) {
        LOG_ERROR( "Failed to create UCP worker: "
                  << ucs_status_string(status));
        return false;
    }

    return true;
}

bool UCXControlServer::CreateListener() {
    // Listener parameters
    ucp_listener_params_t listener_params;
    std::memset(&listener_params, 0, sizeof(listener_params));
    listener_params.field_mask = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR |
                                 UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;
    listener_params.conn_handler.cb = connection_request_callback;
    listener_params.conn_handler.arg = this;

    // Set listen address (must persist for listener lifetime)
    static struct sockaddr_in listen_addr;
    std::memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;

    // Use configured listen address
    if (config_.listen_address == "0.0.0.0") {
        listen_addr.sin_addr.s_addr = INADDR_ANY;  // Listen on all interfaces
    } else {
        if (inet_pton(AF_INET, config_.listen_address.c_str(), &listen_addr.sin_addr) != 1) {
            LOG_ERROR("Invalid listen address: " << config_.listen_address);
            return false;
        }
    }
    listen_addr.sin_port = htons(config_.listen_port);

    // Set sockaddr (using pointer)
    listener_params.sockaddr.addr = (struct sockaddr*)&listen_addr;
    listener_params.sockaddr.addrlen = sizeof(listen_addr);

    // Create listener
    ucs_status_t status = ucp_listener_create(ucp_worker_, &listener_params, &ucp_listener_);
    if (status != UCS_OK) {
        LOG_ERROR( "Failed to create listener: "
                  << ucs_status_string(status));
        return false;
    }

    // Get listen address string
    char addr_str[128];
    snprintf(addr_str, sizeof(addr_str), "%s:%d",
             config_.listen_address.c_str(), config_.listen_port);
    listen_address_ = addr_str;

    return true;
}

//==============================================================================
// Connection Management
//==============================================================================

void UCXControlServer::HandleConnectionRequest(ucp_conn_request_h conn_request, void* arg) {
    (void)arg;

    // Check connection limit
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        if (client_connections_.size() >= config_.max_connections) {
            LOG_ERROR( "Connection limit reached, rejecting request");
            ucp_listener_reject(ucp_listener_, conn_request);
            return;
        }
    }

    AcceptConnection(conn_request);
}

void UCXControlServer::AcceptConnection(ucp_conn_request_h conn_request) {
    // Endpoint parameters
    ucp_ep_params_t ep_params;
    std::memset(&ep_params, 0, sizeof(ep_params));
    ep_params.field_mask = UCP_EP_PARAM_FIELD_CONN_REQUEST;
    ep_params.conn_request = conn_request;

    // Create endpoint
    ucp_ep_h endpoint = nullptr;
    ucs_status_t status = ucp_ep_create(ucp_worker_, &ep_params, &endpoint);
    if (status != UCS_OK) {
        LOG_ERROR( "Failed to create endpoint: "
                  << ucs_status_string(status));
        ucp_listener_reject(ucp_listener_, conn_request);
        return;
    }

    // Generate client ID
    std::string client_id = "client_" + std::to_string(reinterpret_cast<uint64_t>(endpoint));

    // Store connection
    ClientConnection conn;
    conn.endpoint = endpoint;
    conn.client_id = client_id;
    conn.connect_time = std::chrono::system_clock::now();

    // Initialize receive state machine to IDLE
    conn.recv_state.phase = ClientReceiveState::IDLE;
    conn.recv_state.recv_request = nullptr;
    conn.recv_state.msg_length = 0;
    conn.recv_state.bytes_received = 0;

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        client_connections_[client_id] = conn;
    }

    LOG_INFO( "Client connected: " << client_id);
}

void UCXControlServer::CloseConnection(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = client_connections_.find(client_id);
    if (it != client_connections_.end()) {
        if (it->second.endpoint != nullptr) {
            ucp_request_param_t close_param;
            std::memset(&close_param, 0, sizeof(close_param));
            close_param.op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS;
            close_param.flags = UCP_EP_CLOSE_FLAG_FORCE;

            ucs_status_ptr_t request = ucp_ep_close_nbx(it->second.endpoint, &close_param);
            if (request != nullptr && !UCS_PTR_IS_ERR(request)) {
                ucp_request_free(request);
            }
        }
        client_connections_.erase(it);
        LOG_INFO( "Client disconnected: " << client_id);
    }
}

//==============================================================================
// Message Handlers
//==============================================================================

std::string UCXControlServer::HandlePutRequest(const PutRequest& request) {
    PutResponse response;

    // Validate input
    if (request.key().empty() || request.size() == 0) {
        response.set_status_code(-1);
        response.set_error_message("Invalid key or size");
        return response.SerializeAsString();
    }

    // Check memory limit
    {
        std::lock_guard<std::mutex> lock(kv_store_mutex_);
        if (total_memory_used_ + request.size() > config_.max_kv_size) {
            response.set_status_code(-2);
            response.set_error_message("Memory limit exceeded");
            return response.SerializeAsString();
        }
    }

    // Create KV entry
    KVEntry entry;
    entry.dev_ptr = request.dev_ptr();
    entry.size = request.size();
    entry.data_type = request.data_type();
    entry.client_id = request.client_id();
    entry.timestamp = std::chrono::system_clock::now();

    // Store entry
    {
        std::lock_guard<std::mutex> lock(kv_store_mutex_);
        auto it = kv_store_.find(request.key());
        if (it != kv_store_.end()) {
            // Update existing entry
            total_memory_used_ -= it->second.size;
        }
        kv_store_[request.key()] = entry;
        total_memory_used_ += entry.size;
    }

    response.set_status_code(0);
    response.set_error_message("OK");  // Proto3 requires at least one non-default field
    return response.SerializeAsString();
}

std::string UCXControlServer::HandleGetRequest(const GetRequest& request) {
    GetResponse response;

    if (request.key().empty()) {
        response.set_status_code(-1);
        response.set_error_message("Invalid key");
        return response.SerializeAsString();
    }

    std::lock_guard<std::mutex> lock(kv_store_mutex_);
    auto it = kv_store_.find(request.key());
    if (it == kv_store_.end()) {
        response.set_status_code(-3);
        response.set_error_message("Key not found");
        return response.SerializeAsString();
    }

    const KVEntry& entry = it->second;
    response.set_status_code(0);
    response.set_dev_ptr(entry.dev_ptr);
    response.set_size(entry.size);
    response.set_data_type(entry.data_type);

    // TODO: Populate server_root_info with actual HcclRootInfo
    // For now, send empty bytes
    response.set_server_root_info("");

    return response.SerializeAsString();
}

std::string UCXControlServer::HandleDeleteRequest(const DeleteRequest& request) {
    DeleteResponse response;

    if (request.key().empty()) {
        response.set_status_code(-1);
        response.set_error_message("Invalid key");
        return response.SerializeAsString();
    }

    std::lock_guard<std::mutex> lock(kv_store_mutex_);
    auto it = kv_store_.find(request.key());
    if (it == kv_store_.end()) {
        response.set_status_code(-3);
        response.set_error_message("Key not found");
        return response.SerializeAsString();
    }

    total_memory_used_ -= it->second.size;
    kv_store_.erase(it);

    response.set_status_code(0);
    response.set_error_message("OK");  // Proto3 requires at least one non-default field
    return response.SerializeAsString();
}

std::string UCXControlServer::HandleStatsRequest(const ServerStatsRequest& request) {
    (void)request;
    return GetStats().SerializeAsString();
}

bool UCXControlServer::SendResponse(ucp_ep_h endpoint, const std::string& response_data) {
    if (endpoint == nullptr) {
        LOG_ERROR("Invalid endpoint: endpoint is null");
        return false;
    }
    if (response_data.empty()) {
        LOG_ERROR("Empty response data");
        return false;
    }

    // Stack-allocated buffer for send (will persist during limited wait)
    std::vector<char> send_buffer(sizeof(uint32_t) + response_data.size());

    // Write length header (network byte order)
    uint32_t msg_length = htonl(static_cast<uint32_t>(response_data.size()));
    std::memcpy(send_buffer.data(), &msg_length, sizeof(msg_length));

    // Write message body
    std::memcpy(send_buffer.data() + sizeof(uint32_t), response_data.data(), response_data.size());

    // Send complete message (header + body)
    ucp_request_param_t send_param;
    std::memset(&send_param, 0, sizeof(send_param));
    send_param.op_attr_mask = 0;

    void* request = ucp_stream_send_nbx(
        endpoint,
        send_buffer.data(),
        send_buffer.size(),
        &send_param);

    // Handle result
    if (UCS_PTR_IS_ERR(request)) {
        LOG_ERROR("Failed to initiate send: " << ucs_status_string(UCS_PTR_STATUS(request)));
        return false;
    } else if (UCS_PTR_IS_PTR(request)) {
        // Request is pending, do limited progress to help it complete
        // But don't wait forever to avoid deadlock
        int progress_count = 0;
        const int max_progress = 100;  // Limit iterations
        ucs_status_t status;

        do {
            ucp_worker_progress(ucp_worker_);
            status = ucp_request_check_status(request);
            progress_count++;
        } while (status == UCS_INPROGRESS && progress_count < max_progress);

        ucp_request_free(request);

        if (status != UCS_OK && status != UCS_INPROGRESS) {
            LOG_ERROR("Send failed: " << ucs_status_string(status));
            return false;
        }
        // If still in progress after max iterations, that's okay
        // UCX will complete it in background
    }
    // Completed immediately (request == NULL)

    LOG_DEBUG("Response send completed, size=" << response_data.size());
    return true;
}

//==============================================================================
// Message Receive State Machine
//==============================================================================

void UCXControlServer::ProcessClientReceive(ClientConnection& conn) {
    auto& state = conn.recv_state;

    LOG_DEBUG("ProcessClientReceive: client=" << conn.client_id << ", phase=" << static_cast<int>(state.phase));

    switch (state.phase) {
        case ClientReceiveState::IDLE:
            // Start receiving message length header
            StartReceiveLength(conn);
            break;

        case ClientReceiveState::RECV_LENGTH:
            // Check if length header is complete
            if (CheckReceiveComplete(state.recv_request, 4, state.bytes_received)) {
                // Verify we received exactly 4 bytes
                if (state.bytes_received != 4) {
                    LOG_ERROR("Incomplete length header: " << state.bytes_received
                              << " bytes, expected 4, from " << conn.client_id);
                    // Need to continue receiving
                    state.recv_request = nullptr;
                    state.phase = ClientReceiveState::IDLE;
                    return;
                }

                // Parse message length
                state.msg_length = ntohl(*reinterpret_cast<uint32_t*>(state.buffer.data()));
                LOG_DEBUG("Received length header: " << state.msg_length << " from " << conn.client_id);

                // Validate message size
                if (state.msg_length == 0 || state.msg_length > MAX_MESSAGE_SIZE) {
                    LOG_ERROR("Invalid message length: " << state.msg_length << " from " << conn.client_id);
                    // Mark for close (will be handled by ProgressWorker)
                    conn.endpoint = nullptr;
                    return;
                }

                // Start receiving message body
                StartReceiveBody(conn);
            }
            break;

        case ClientReceiveState::RECV_BODY:
            // Check if message body is complete
            if (CheckReceiveComplete(state.recv_request, state.msg_length, state.bytes_received)) {
                LOG_DEBUG("Message received from " << conn.client_id << ", size=" << state.msg_length);

                // Dispatch message and get response
                std::string response = DispatchMessage(state.buffer);

                // Send response back to client
                if (!SendResponse(conn.endpoint, response)) {
                    LOG_ERROR("Failed to send response to " << conn.client_id);
                    // Mark for close (will be handled by ProgressWorker)
                    conn.endpoint = nullptr;
                    return;
                }

                // Reset to IDLE for next message
                state.phase = ClientReceiveState::IDLE;
                state.msg_length = 0;
                state.bytes_received = 0;
                state.buffer.clear();
            }
            break;
    }
}

void UCXControlServer::StartReceiveLength(ClientConnection& conn) {
    auto& state = conn.recv_state;

    // Allocate buffer for length header (4 bytes)
    state.buffer.resize(4);
    state.bytes_received = 0;

    // Start async receive
    ucp_request_param_t recv_param;
    std::memset(&recv_param, 0, sizeof(recv_param));
    recv_param.op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS;
    recv_param.flags = 0;

    state.recv_request = ucp_stream_recv_nbx(
        conn.endpoint,
        state.buffer.data(),
        state.buffer.size(),
        &state.bytes_received,
        &recv_param);

    if (UCS_PTR_IS_ERR(state.recv_request)) {
        LOG_ERROR("Failed to start receive length: "
                  << ucs_status_string(UCS_PTR_STATUS(state.recv_request))
                  << " from " << conn.client_id);
        // Mark for close
        conn.endpoint = nullptr;
        return;
    }

    state.phase = ClientReceiveState::RECV_LENGTH;
}

void UCXControlServer::StartReceiveBody(ClientConnection& conn) {
    auto& state = conn.recv_state;

    // Allocate buffer for message body
    state.buffer.resize(state.msg_length);
    state.bytes_received = 0;

    // Start async receive
    ucp_request_param_t recv_param;
    std::memset(&recv_param, 0, sizeof(recv_param));
    recv_param.op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS;
    recv_param.flags = 0;

    state.recv_request = ucp_stream_recv_nbx(
        conn.endpoint,
        state.buffer.data(),
        state.buffer.size(),
        &state.bytes_received,
        &recv_param);

    if (UCS_PTR_IS_ERR(state.recv_request)) {
        LOG_ERROR("Failed to start receive body: "
                  << ucs_status_string(UCS_PTR_STATUS(state.recv_request))
                  << " from " << conn.client_id);
        // Mark for close
        conn.endpoint = nullptr;
        return;
    }

    state.phase = ClientReceiveState::RECV_BODY;
}

bool UCXControlServer::CheckReceiveComplete(void* request, size_t expected_size, size_t& bytes_received) {
    if (request == nullptr) {
        // Immediate completion (UCS_OK)
        return bytes_received >= expected_size;
    }

    if (UCS_PTR_IS_PTR(request)) {
        // Check if request is complete
        ucs_status_t status = ucp_request_check_status(request);

        if (status == UCS_OK) {
            // Request completed, free it first
            ucp_request_free(request);
            // Then check if we received the expected number of bytes
            return bytes_received >= expected_size;
        } else if (status == UCS_INPROGRESS) {
            // Still in progress
            return false;
        } else {
            // Error occurred
            LOG_ERROR("Receive error: " << ucs_status_string(status));
            ucp_request_free(request);
            return false;
        }
    }

    // Should not reach here
    return false;
}

std::string UCXControlServer::DispatchMessage(const std::vector<char>& message_data) {
    LOG_DEBUG("Dispatching message, size=" << message_data.size());

    // Try to parse as PutRequest first
    PutRequest put_req;
    if (put_req.ParseFromArray(message_data.data(), message_data.size())) {
        if (!put_req.key().empty()) {
            LOG_DEBUG("Parsed as PutRequest, key=" << put_req.key());
            std::string response = HandlePutRequest(put_req);
            LOG_DEBUG("HandlePutRequest returned response, size=" << response.size());
            return response;
        }
    }

    // Try GetRequest
    GetRequest get_req;
    if (get_req.ParseFromArray(message_data.data(), message_data.size())) {
        if (!get_req.key().empty()) {
            return HandleGetRequest(get_req);
        }
    }

    // Try DeleteRequest
    DeleteRequest del_req;
    if (del_req.ParseFromArray(message_data.data(), message_data.size())) {
        if (!del_req.key().empty()) {
            return HandleDeleteRequest(del_req);
        }
    }

    // Try ServerStatsRequest
    ServerStatsRequest stats_req;
    if (stats_req.ParseFromArray(message_data.data(), message_data.size())) {
        return HandleStatsRequest(stats_req);
    }

    // Unknown message type
    LOG_ERROR("Failed to parse message, size=" << message_data.size());
    PutResponse error_response;
    error_response.set_status_code(-1);
    error_response.set_error_message("Unknown message type");
    return error_response.SerializeAsString();
}

//==============================================================================
// Worker Progress
//==============================================================================

void UCXControlServer::ProgressWorker() {
    if (ucp_worker_ != nullptr) {
        // Drive UCX event handling
        ucp_worker_progress(ucp_worker_);

        // Collect clients to close (to avoid modifying map while iterating)
        std::vector<std::string> clients_to_close;

        // Process all client receive states
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            static int log_counter = 0;
            if (++log_counter % 1000 == 0 && !client_connections_.empty()) {
                LOG_DEBUG("ProgressWorker: " << client_connections_.size() << " clients");
            }
            for (auto& [client_id, conn] : client_connections_) {
                // Check if client should be closed (endpoint is null)
                if (conn.endpoint == nullptr) {
                    clients_to_close.push_back(client_id);
                    continue;
                }

                ProcessClientReceive(conn);

                // If endpoint was invalidated during processing, mark for close
                if (conn.endpoint == nullptr) {
                    clients_to_close.push_back(client_id);
                }
            }
        }

        // Close clients outside the lock
        for (const auto& client_id : clients_to_close) {
            CloseConnection(client_id);
        }
    }
}

}  // namespace zerokv
