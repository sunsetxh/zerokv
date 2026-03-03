#include "zerokv/server.h"
#include "zerokv/storage.h"
#include "zerokv/protocol.h"
#include <ucp/api/ucp.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <memory>
#include <cstring>

namespace zerokv {

Server::Server() : running_(false),
      context_(nullptr), worker_(nullptr), listener_(nullptr) {
}

Server::~Server() {
    stop();
}

Status Server::start(const std::string& addr, uint16_t port, size_t max_memory) {
    addr_ = addr;
    port_ = port;

    // Initialize storage
    storage_ = std::make_unique<StorageEngine>(max_memory);

    // Initialize UCX
    Status status = initialize_ucx();
    if (status != Status::OK) {
        std::cerr << "Failed to initialize UCX" << std::endl;
        return status;
    }

    status = create_listener();
    if (status != Status::OK) {
        std::cerr << "Failed to create listener" << std::endl;
        return status;
    }

    running_ = true;
    worker_thread_ = std::thread([this]() { run(); });

    std::cout << "ZeroKV Server started on " << addr_ << ":" << port_ << std::endl;
    return Status::OK;
}

Status Server::initialize_ucx() {
    ucp_config_t* config;
    ucp_params_t params;
    ucs_status_t status;

    status = ucp_config_read("ZeroKV", nullptr, &config);
    if (status != UCS_OK) return Status::ERROR;

    params.field_mask = UCP_PARAM_FIELD_FEATURES |
                       UCP_PARAM_FIELD_REQUEST_SIZE;
    params.features = UCP_FEATURE_TAG | UCP_FEATURE_RMA;
    params.request_size = 256;

    status = ucp_init(&params, config, &context_);
    ucp_config_release(config);

    if (status != UCS_OK) return Status::ERROR;

    ucp_worker_params_t worker_params;
    worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_MULTI;

    status = ucp_worker_create(context_, &worker_params, &worker_);
    if (status != UCS_OK) {
        ucp_cleanup(context_);
        return Status::ERROR;
    }

    return Status::OK;
}

Status Server::create_listener() {
    // Note: Simplified - need proper address handling
    return Status::OK;
}

void Server::process_request(const std::vector<uint8_t>& request,
                             std::vector<uint8_t>& response) {
    RequestHeader req_header;
    std::string key;
    void* value = nullptr;

    if (!ProtocolCodec::decode_request(request, req_header, key, value)) {
        response = ProtocolCodec::encode_response(
            Status::ERROR, req_header.request_id);
        return;
    }

    switch (static_cast<OpCode>(req_header.opcode)) {
        case OpCode::PUT: {
            Status status = storage_->put(key, value, req_header.value_len);
            response = ProtocolCodec::encode_response(status, req_header.request_id);
            break;
        }
        case OpCode::GET: {
            std::vector<uint8_t> buffer(req_header.value_len);
            size_t size = buffer.size();
            Status status = storage_->get(key, buffer.data(), &size);
            if (status == Status::OK) {
                response = ProtocolCodec::encode_response(
                    status, req_header.request_id, buffer.data(), size);
            } else {
                response = ProtocolCodec::encode_response(
                    status, req_header.request_id);
            }
            break;
        }
        case OpCode::DELETE: {
            Status status = storage_->delete_key(key);
            response = ProtocolCodec::encode_response(status, req_header.request_id);
            break;
        }
        default:
            response = ProtocolCodec::encode_response(
                Status::ERROR, req_header.request_id);
            break;
    }
}

void Server::handle_request(ucp_worker_h worker, ucp_ep_h ep) {
    // Simplified request handler
    // In production: proper tag matching, request tracking
}

void Server::run() {
    while (running_) {
        ucp_worker_progress(worker_);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Server::stop() {
    running_ = false;
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    if (listener_) {
        ucp_listener_destroy(listener_);
    }
    if (worker_) {
        ucp_worker_destroy(worker_);
    }
    if (context_) {
        ucp_cleanup(context_);
    }
    std::cout << "ZeroKV Server stopped" << std::endl;
}

} // namespace zerokv
