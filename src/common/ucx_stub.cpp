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
 * @file ucx_stub.cpp
 * @brief Minimal UCX API stub implementation
 */

#include "ucx_stub.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <vector>

// Stub implementation structures
struct ucp_context {
    bool initialized = true;
};

struct ucp_worker {
    ucp_context_h context;
    std::vector<uint8_t> address_data;
    std::queue<std::vector<uint8_t>> recv_queue;
    std::mutex queue_mutex;
};

struct ucp_ep {
    ucp_worker_h worker;
    std::vector<uint8_t> remote_address;
};

struct ucp_listener {
    ucp_worker_h worker;
    uint16_t port;
};

struct ucp_conn_request {
    ucp_listener_h listener;
};

struct ucp_config {
    std::map<std::string, std::string> settings;
};

extern "C" {

const char* ucs_status_string(ucs_status_t status) {
    switch (status) {
        case UCS_OK: return "Success";
        case UCS_INPROGRESS: return "Operation in progress";
        case UCS_ERR_NO_MEMORY: return "Out of memory";
        case UCS_ERR_INVALID_PARAM: return "Invalid parameter";
        case UCS_ERR_NO_RESOURCE: return "No resource available";
        case UCS_ERR_IO_ERROR: return "I/O error";
        case UCS_ERR_NOT_IMPLEMENTED: return "Not implemented";
        default: return "Unknown error";
    }
}

ucs_status_t ucp_config_read(const char* env_prefix, const char* filename,
                             ucp_config_t** config_p) {
    (void)env_prefix;
    (void)filename;

    if (config_p == nullptr) {
        return UCS_ERR_INVALID_PARAM;
    }

    *config_p = new ucp_config;
    return UCS_OK;
}

void ucp_config_release(ucp_config_t* config) {
    delete config;
}

ucs_status_t ucp_config_modify(ucp_config_t* config, const char* name,
                               const char* value) {
    if (config == nullptr || name == nullptr || value == nullptr) {
        return UCS_ERR_INVALID_PARAM;
    }

    config->settings[name] = value;
    return UCS_OK;
}

ucs_status_t ucp_init(const ucp_params_t* params, const ucp_config_t* config,
                      ucp_context_h* context_p) {
    (void)params;
    (void)config;

    if (context_p == nullptr) {
        return UCS_ERR_INVALID_PARAM;
    }

    *context_p = new ucp_context();
    return UCS_OK;
}

void ucp_cleanup(ucp_context_h context) {
    delete context;
}

ucs_status_t ucp_worker_create(ucp_context_h context,
                               const ucp_worker_params_t* params,
                               ucp_worker_h* worker_p) {
    (void)params;

    if (context == nullptr || worker_p == nullptr) {
        return UCS_ERR_INVALID_PARAM;
    }

    ucp_worker_h worker = new ucp_worker();
    worker->context = context;

    // Generate a simple address (just some random bytes for stub)
    worker->address_data.resize(64);
    for (size_t i = 0; i < 64; ++i) {
        worker->address_data[i] = static_cast<uint8_t>(i + rand() % 256);
    }

    *worker_p = worker;
    return UCS_OK;
}

void ucp_worker_destroy(ucp_worker_h worker) {
    delete worker;
}

unsigned ucp_worker_progress(ucp_worker_h worker) {
    (void)worker;
    // Stub: just return 0 (no work done)
    return 0;
}

ucs_status_t ucp_worker_get_address(ucp_worker_h worker,
                                    ucp_address_t** address_p,
                                    size_t* address_length_p) {
    if (worker == nullptr || address_p == nullptr || address_length_p == nullptr) {
        return UCS_ERR_INVALID_PARAM;
    }

    // Allocate and copy address
    void* addr = malloc(worker->address_data.size());
    if (addr == nullptr) {
        return UCS_ERR_NO_MEMORY;
    }

    memcpy(addr, worker->address_data.data(), worker->address_data.size());
    *address_p = reinterpret_cast<ucp_address_t*>(addr);
    *address_length_p = worker->address_data.size();

    return UCS_OK;
}

void ucp_worker_release_address(ucp_worker_h worker, ucp_address_t* address) {
    (void)worker;
    free(address);
}

ucs_status_t ucp_ep_create(ucp_worker_h worker, const ucp_ep_params_t* params,
                           ucp_ep_h* ep_p) {
    if (worker == nullptr || params == nullptr || ep_p == nullptr) {
        return UCS_ERR_INVALID_PARAM;
    }

    ucp_ep_h ep = new ucp_ep();
    ep->worker = worker;

    // Store remote address (stub)
    if (params->address != nullptr) {
        const uint8_t* addr_bytes = reinterpret_cast<const uint8_t*>(params->address);
        ep->remote_address.assign(addr_bytes, addr_bytes + 64);
    }

    *ep_p = ep;
    return UCS_OK;
}

ucs_status_ptr_t ucp_ep_close_nbx(ucp_ep_h ep, const ucp_request_param_t* param) {
    (void)param;

    if (ep == nullptr) {
        return reinterpret_cast<ucs_status_ptr_t>(UCS_ERR_INVALID_PARAM);
    }

    delete ep;

    // Return nullptr to indicate immediate completion
    return nullptr;
}

ucs_status_ptr_t ucp_am_send_nbx(ucp_ep_h ep, unsigned id,
                                  const void* header, size_t header_length,
                                  const void* buffer, size_t count,
                                  const ucp_request_param_t* param) {
    (void)id;
    (void)header;
    (void)header_length;
    (void)param;

    if (ep == nullptr || buffer == nullptr) {
        return reinterpret_cast<ucs_status_ptr_t>(UCS_ERR_INVALID_PARAM);
    }

    // Stub: Store data in worker's receive queue (loopback for testing)
    std::lock_guard<std::mutex> lock(ep->worker->queue_mutex);
    std::vector<uint8_t> data(count);
    memcpy(data.data(), buffer, count);
    ep->worker->recv_queue.push(std::move(data));

    // Return nullptr to indicate immediate completion
    return nullptr;
}

ucs_status_t ucp_worker_set_am_recv_handler(ucp_worker_h worker,
                                             const ucp_am_handler_param_t* param) {
    (void)worker;
    (void)param;

    if (worker == nullptr || param == nullptr) {
        return UCS_ERR_INVALID_PARAM;
    }

    // Stub: Accept handler registration
    return UCS_OK;
}

ucs_status_t ucp_request_check_status(void* request) {
    if (request == nullptr) {
        return UCS_OK;  // nullptr means already completed
    }

    // Stub: Always return completed
    return UCS_OK;
}

void ucp_request_free(void* request) {
    // Stub: Nothing to free since we return nullptr for immediate completion
    (void)request;
}

ucs_status_t ucp_listener_create(ucp_worker_h worker, const ucp_listener_params_t* params,
                                ucp_listener_h* listener_p) {
    (void)worker;
    (void)params;

    if (listener_p == nullptr) {
        return UCS_ERR_INVALID_PARAM;
    }

    // Stub: Create a mock listener
    *listener_p = new ucp_listener();
    return UCS_OK;
}

void ucp_listener_destroy(ucp_listener_h listener) {
    delete listener;
}

void ucp_listener_reject(ucp_listener_h listener, ucp_conn_request_h conn_request) {
    (void)listener;
    (void)conn_request;
    // Stub: Do nothing for now
}

// Stream API implementations
ucs_status_ptr_t ucp_stream_send_nbx(ucp_ep_h ep, const void* buffer, size_t count,
                                      const ucp_request_param_t* param) {
    (void)ep;
    (void)buffer;
    (void)count;
    (void)param;

    // Stub: Return nullptr to indicate immediate completion
    return nullptr;
}

ucs_status_ptr_t ucp_stream_recv_nbx(ucp_ep_h ep, void* buffer, size_t count,
                                      size_t* length, const ucp_request_param_t* param) {
    (void)ep;
    (void)buffer;
    (void)count;
    (void)param;

    // Stub: Return 0 length (no data available)
    if (length != nullptr) {
        *length = 0;
    }

    // Return nullptr to indicate immediate completion
    return nullptr;
}

}  // extern "C"
