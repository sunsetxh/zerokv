/**
 * Copyright (c) 2025. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include <zerokv/ucx_control_client.h>
#include <zerokv/logger.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <cstring>
#include <thread>

namespace zerokv {

// ============================================================================
// UCXControlClient Implementation
// ============================================================================

UCXControlClient::UCXControlClient(const UCXClientConfig& config)
    : config_(config) {
}

UCXControlClient::~UCXControlClient() {
    Disconnect();

    if (ucp_worker_) {
        ucp_worker_destroy(ucp_worker_);
        ucp_worker_ = nullptr;
    }

    if (ucp_context_) {
        ucp_cleanup(ucp_context_);
        ucp_context_ = nullptr;
    }
}

bool UCXControlClient::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_) {
        LOG_WARN("UCXControlClient already initialized");
        return true;
    }

    LOG_INFO("Initializing UCX control client");

    if (!CreateUCPContext()) {
        LOG_ERROR("Failed to create UCP context");
        return false;
    }

    if (!CreateUCPWorker()) {
        LOG_ERROR("Failed to create UCP worker");
        return false;
    }

    initialized_ = true;
    LOG_INFO("UCX control client initialized successfully");
    return true;
}

bool UCXControlClient::Connect(const std::string& server_address, uint16_t server_port) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        LOG_ERROR("Client not initialized");
        return false;
    }

    if (connected_) {
        LOG_WARN("Already connected to server");
        return true;
    }

    LOG_INFO("Connecting to server " << server_address << ":" << server_port);

    if (!CreateEndpoint(server_address, server_port)) {
        LOG_ERROR("Failed to create endpoint to server");
        return false;
    }

    server_address_ = server_address;
    server_port_ = server_port;
    connected_ = true;

    LOG_INFO("Successfully connected to server");
    return true;
}

void UCXControlClient::Disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!connected_) {
        return;
    }

    LOG_INFO("Disconnecting from server");

    if (ucp_endpoint_) {
        ucp_request_param_t param;
        param.op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS;
        param.flags = UCP_EP_CLOSE_FLAG_FORCE;

        ucs_status_ptr_t status_ptr = ucp_ep_close_nbx(ucp_endpoint_, &param);
        if (UCS_PTR_IS_PTR(status_ptr)) {
            // Wait for close to complete
            ucs_status_t status;
            do {
                ucp_worker_progress(ucp_worker_);
                status = ucp_request_check_status(status_ptr);
            } while (status == UCS_INPROGRESS);
            ucp_request_free(status_ptr);
        }

        ucp_endpoint_ = nullptr;
    }

    connected_ = false;
    server_address_.clear();
    server_port_ = 0;

    LOG_INFO("Disconnected from server");
}

RPCResult<PutResponse> UCXControlClient::Put(const PutRequest& request) {
    return SendRequest<PutRequest, PutResponse>(request, "Put");
}

RPCResult<GetResponse> UCXControlClient::Get(const GetRequest& request) {
    return SendRequest<GetRequest, GetResponse>(request, "Get");
}

RPCResult<DeleteResponse> UCXControlClient::Delete(const DeleteRequest& request) {
    return SendRequest<DeleteRequest, DeleteResponse>(request, "Delete");
}

RPCResult<ServerStatsResponse> UCXControlClient::GetStats(const ServerStatsRequest& request) {
    return SendRequest<ServerStatsRequest, ServerStatsResponse>(request, "Stats");
}

// ============================================================================
// Private Methods
// ============================================================================

bool UCXControlClient::CreateUCPContext() {
    ucp_params_t ucp_params;
    ucp_config_t* config;
    ucs_status_t status;

    // Read UCP configuration
    status = ucp_config_read(nullptr, nullptr, &config);
    if (status != UCS_OK) {
        LOG_ERROR("Failed to read UCP config: " << ucs_status_string(status));
        return false;
    }

    // Set UCP parameters
    std::memset(&ucp_params, 0, sizeof(ucp_params));
    ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES |
                            UCP_PARAM_FIELD_REQUEST_SIZE |
                            UCP_PARAM_FIELD_REQUEST_INIT;

    ucp_params.features = UCP_FEATURE_TAG |
                          UCP_FEATURE_STREAM |
                          UCP_FEATURE_AM;

    ucp_params.request_size = sizeof(ucp_request_param_t);
    ucp_params.request_init = nullptr;

    // Create UCP context
    status = ucp_init(&ucp_params, config, &ucp_context_);
    ucp_config_release(config);

    if (status != UCS_OK) {
        LOG_ERROR("Failed to create UCP context: " << ucs_status_string(status));
        return false;
    }

    LOG_DEBUG("UCP context created successfully");
    return true;
}

bool UCXControlClient::CreateUCPWorker() {
    ucp_worker_params_t worker_params;
    ucs_status_t status;

    std::memset(&worker_params, 0, sizeof(worker_params));
    worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;

    status = ucp_worker_create(ucp_context_, &worker_params, &ucp_worker_);
    if (status != UCS_OK) {
        LOG_ERROR("Failed to create UCP worker: " << ucs_status_string(status));
        return false;
    }

    LOG_DEBUG("UCP worker created successfully");
    return true;
}

bool UCXControlClient::CreateEndpoint(const std::string& server_address, uint16_t server_port) {
    ucp_ep_params_t ep_params;
    ucs_status_t status;

    // Resolve server address
    struct addrinfo hints, *res;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int ret = getaddrinfo(server_address.c_str(), std::to_string(server_port).c_str(),
                         &hints, &res);
    if (ret != 0) {
        LOG_ERROR("Failed to resolve server address: " << gai_strerror(ret));
        return false;
    }

    // Create endpoint parameters
    std::memset(&ep_params, 0, sizeof(ep_params));
    ep_params.field_mask = UCP_EP_PARAM_FIELD_FLAGS |
                          UCP_EP_PARAM_FIELD_SOCK_ADDR |
                          UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;

    ep_params.flags = UCP_EP_PARAMS_FLAGS_CLIENT_SERVER;
    ep_params.sockaddr.addr = res->ai_addr;
    ep_params.sockaddr.addrlen = res->ai_addrlen;
    ep_params.err_mode = UCP_ERR_HANDLING_MODE_NONE;

    // Create endpoint
    status = ucp_ep_create(ucp_worker_, &ep_params, &ucp_endpoint_);
    freeaddrinfo(res);

    if (status != UCS_OK) {
        LOG_ERROR("Failed to create endpoint: " << ucs_status_string(status));
        return false;
    }

    LOG_DEBUG("Endpoint created successfully");
    return true;
}

template<typename RequestT, typename ResponseT>
RPCResult<ResponseT> UCXControlClient::SendRequest(const RequestT& request,
                                                   const std::string& request_type) {
    RPCResult<ResponseT> result;
    result.status = RPCStatus::SUCCESS;

    std::lock_guard<std::mutex> lock(mutex_);

    if (!connected_) {
        result.status = RPCStatus::NOT_CONNECTED;
        result.error_message = "Client not connected to server";
        LOG_ERROR(result.error_message);
        return result;
    }

    // Serialize request
    std::string request_data;
    if (!request.SerializeToString(&request_data)) {
        result.status = RPCStatus::SERVER_ERROR;
        result.error_message = "Failed to serialize request";
        LOG_ERROR(result.error_message);
        return result;
    }

    LOG_DEBUG("Sending " << request_type << " request, size=" << request_data.size());

    // Send request with retry
    bool sent = false;
    for (uint32_t attempt = 0; attempt <= config_.max_retries; ++attempt) {
        if (attempt > 0) {
            LOG_WARN("Retrying " << request_type << " request, attempt " << attempt);
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.retry_delay_ms));
        }

        if (SendMessage(request_data)) {
            sent = true;
            break;
        }
    }

    if (!sent) {
        result.status = RPCStatus::NETWORK_ERROR;
        result.error_message = "Failed to send request after retries";
        LOG_ERROR(result.error_message);
        return result;
    }

    // Receive response with timeout
    std::string response_data;
    if (!ReceiveResponse(response_data, config_.request_timeout_ms)) {
        result.status = RPCStatus::TIMEOUT;
        result.error_message = "Request timeout";
        LOG_ERROR(result.error_message);
        return result;
    }

    // Deserialize response
    if (!result.response.ParseFromString(response_data)) {
        result.status = RPCStatus::INVALID_RESPONSE;
        result.error_message = "Failed to parse response";
        LOG_ERROR(result.error_message);
        return result;
    }

    LOG_DEBUG(request_type << " request completed successfully");
    return result;
}

bool UCXControlClient::SendMessage(const std::string& message) {
    if (message.size() > MAX_MESSAGE_SIZE) {
        LOG_ERROR("Message too large: " << message.size() << " > " << MAX_MESSAGE_SIZE);
        return false;
    }

    // Send message size first (4 bytes, network byte order)
    uint32_t msg_size = htonl(static_cast<uint32_t>(message.size()));
    ucp_request_param_t param;
    std::memset(&param, 0, sizeof(param));
    param.op_attr_mask = 0;  // No flags needed for stream send

    // Send size
    ucs_status_ptr_t status_ptr = ucp_stream_send_nbx(
        ucp_endpoint_, &msg_size, sizeof(msg_size), &param);

    if (UCS_PTR_IS_ERR(status_ptr)) {
        LOG_ERROR("Failed to send message size: " <<
                 ucs_status_string(UCS_PTR_STATUS(status_ptr)));
        return false;
    }

    if (UCS_PTR_IS_PTR(status_ptr)) {
        // Wait for completion
        ucs_status_t status;
        do {
            ucp_worker_progress(ucp_worker_);
            status = ucp_request_check_status(status_ptr);
        } while (status == UCS_INPROGRESS);

        ucp_request_free(status_ptr);

        if (status != UCS_OK) {
            LOG_ERROR("Failed to send message size: " << ucs_status_string(status));
            return false;
        }
    }

    // Send message data
    status_ptr = ucp_stream_send_nbx(
        ucp_endpoint_, message.data(), message.size(), &param);

    if (UCS_PTR_IS_ERR(status_ptr)) {
        LOG_ERROR("Failed to send message: " <<
                 ucs_status_string(UCS_PTR_STATUS(status_ptr)));
        return false;
    }

    if (UCS_PTR_IS_PTR(status_ptr)) {
        // Wait for completion
        ucs_status_t status;
        do {
            ucp_worker_progress(ucp_worker_);
            status = ucp_request_check_status(status_ptr);
        } while (status == UCS_INPROGRESS);

        ucp_request_free(status_ptr);

        if (status != UCS_OK) {
            LOG_ERROR("Failed to send message: " << ucs_status_string(status));
            return false;
        }
    }

    return true;
}

bool UCXControlClient::ReceiveResponse(std::string& response, uint32_t timeout_ms) {
    auto start_time = std::chrono::steady_clock::now();
    auto timeout = std::chrono::milliseconds(timeout_ms);

    // Receive message size first (4 bytes)
    uint32_t msg_size_net = 0;
    size_t received = 0;
    ucp_request_param_t param;
    std::memset(&param, 0, sizeof(param));

    while (received < sizeof(msg_size_net)) {
        if (std::chrono::steady_clock::now() - start_time > timeout) {
            LOG_ERROR("Timeout waiting for response size");
            return false;
        }

        size_t length = 0;
        ucs_status_ptr_t status_ptr = ucp_stream_recv_nbx(
            ucp_endpoint_,
            reinterpret_cast<char*>(&msg_size_net) + received,
            sizeof(msg_size_net) - received,
            &length,
            &param);

        if (UCS_PTR_IS_ERR(status_ptr)) {
            LOG_ERROR("Failed to receive message size: " <<
                     ucs_status_string(UCS_PTR_STATUS(status_ptr)));
            return false;
        }

        if (UCS_PTR_IS_PTR(status_ptr)) {
            // Wait for completion
            ucs_status_t status;
            do {
                ucp_worker_progress(ucp_worker_);
                status = ucp_request_check_status(status_ptr);
            } while (status == UCS_INPROGRESS);

            ucp_request_free(status_ptr);

            if (status != UCS_OK) {
                LOG_ERROR("Failed to receive message size: " << ucs_status_string(status));
                return false;
            }
        }

        received += length;

        if (length == 0) {
            ucp_worker_progress(ucp_worker_);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    uint32_t msg_size = ntohl(msg_size_net);
    if (msg_size > MAX_MESSAGE_SIZE) {
        LOG_ERROR("Response too large: " << msg_size << " > " << MAX_MESSAGE_SIZE);
        return false;
    }

    // Receive message data
    response.resize(msg_size);
    received = 0;

    while (received < msg_size) {
        if (std::chrono::steady_clock::now() - start_time > timeout) {
            LOG_ERROR("Timeout waiting for response data");
            return false;
        }

        size_t length = 0;
        ucs_status_ptr_t status_ptr = ucp_stream_recv_nbx(
            ucp_endpoint_,
            &response[received],
            msg_size - received,
            &length,
            &param);

        if (UCS_PTR_IS_ERR(status_ptr)) {
            LOG_ERROR("Failed to receive message: " <<
                     ucs_status_string(UCS_PTR_STATUS(status_ptr)));
            return false;
        }

        if (UCS_PTR_IS_PTR(status_ptr)) {
            // Wait for completion
            ucs_status_t status;
            do {
                ucp_worker_progress(ucp_worker_);
                status = ucp_request_check_status(status_ptr);
            } while (status == UCS_INPROGRESS);

            ucp_request_free(status_ptr);

            if (status != UCS_OK) {
                LOG_ERROR("Failed to receive message: " << ucs_status_string(status));
                return false;
            }
        }

        received += length;

        if (length == 0) {
            ucp_worker_progress(ucp_worker_);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    return true;
}

void UCXControlClient::ProgressWorker() {
    if (ucp_worker_) {
        ucp_worker_progress(ucp_worker_);
    }
}

}  // namespace zerokv
