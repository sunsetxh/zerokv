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

#include <cstring>
#include <iostream>
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
        std::cerr << "Server already initialized" << std::endl;
        return false;
    }

    // Create UCP context
    if (!CreateUCPContext()) {
        std::cerr << "Failed to create UCP context" << std::endl;
        return false;
    }

    // Create UCP worker
    if (!CreateUCPWorker()) {
        std::cerr << "Failed to create UCP worker" << std::endl;
        return false;
    }

    // Create listener
    if (!CreateListener()) {
        std::cerr << "Failed to create listener" << std::endl;
        return false;
    }

    initialized_ = true;
    std::cout << "UCX Control Server initialized successfully" << std::endl;
    std::cout << "Listen address: " << GetListenAddress() << std::endl;

    return true;
}

bool UCXControlServer::Start() {
    if (!initialized_) {
        std::cerr << "Server not initialized" << std::endl;
        return false;
    }

    running_ = true;
    start_time_ = std::chrono::system_clock::now();
    std::cout << "UCX Control Server started" << std::endl;

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
    std::cout << "UCX Control Server stopped" << std::endl;
}

bool UCXControlServer::Run(uint32_t stop_after_ms) {
    if (!running_) {
        std::cerr << "Server not running" << std::endl;
        return false;
    }

    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::milliseconds(stop_after_ms);

    std::cout << "Server event loop started" << std::endl;

    while (running_) {
        // Progress UCX worker to handle communication
        ProgressWorker();

        // Check timeout
        if (stop_after_ms > 0) {
            auto now = std::chrono::steady_clock::now();
            if (now >= end_time) {
                std::cout << "Server run timeout reached" << std::endl;
                break;
            }
        }

        // Small sleep to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::cout << "Server event loop ended" << std::endl;
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
        std::cerr << "Failed to read UCP config" << std::endl;
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
        std::cerr << "Failed to initialize UCP context: "
                  << ucs_status_string(status) << std::endl;
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
        std::cerr << "Failed to create UCP worker: "
                  << ucs_status_string(status) << std::endl;
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
    listen_addr.sin_addr.s_addr = INADDR_ANY;  // Listen on all interfaces
    listen_addr.sin_port = htons(config_.listen_port);

    // Set sockaddr (using pointer)
    listener_params.sockaddr.addr = (struct sockaddr*)&listen_addr;
    listener_params.sockaddr.addrlen = sizeof(listen_addr);

    // Create listener
    ucs_status_t status = ucp_listener_create(ucp_worker_, &listener_params, &ucp_listener_);
    if (status != UCS_OK) {
        std::cerr << "Failed to create listener: "
                  << ucs_status_string(status) << std::endl;
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
            std::cerr << "Connection limit reached, rejecting request" << std::endl;
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
        std::cerr << "Failed to create endpoint: "
                  << ucs_status_string(status) << std::endl;
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

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        client_connections_[client_id] = conn;
    }

    std::cout << "Client connected: " << client_id << std::endl;
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
        std::cout << "Client disconnected: " << client_id << std::endl;
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
    return response.SerializeAsString();
}

std::string UCXControlServer::HandleStatsRequest(const ServerStatsRequest& request) {
    (void)request;
    return GetStats().SerializeAsString();
}

bool UCXControlServer::SendResponse(ucp_ep_h endpoint, const std::string& response_data) {
    // For now, this is a placeholder
    // Full implementation will use UCX stream or active message
    (void)endpoint;
    (void)response_data;
    return true;
}

void UCXControlServer::ProgressWorker() {
    if (ucp_worker_ != nullptr) {
        ucp_worker_progress(ucp_worker_);
    }
}

}  // namespace zerokv
