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
 * @file p2p_ucx_mock.cpp
 * @brief P2P-Transfer API mock implementation using UCX
 */

#include "p2p_ucx_mock.h"

#ifdef USE_UCX_STUB
#include "ucx_stub.h"
#else
#include <ucp/api/ucp.h>
#endif

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <iostream>

namespace zerokv {

// Global UCX context and worker
struct UCXContext {
    ucp_context_h context = nullptr;
    ucp_worker_h worker = nullptr;
    bool initialized = false;
    std::mutex mutex;
};

static UCXContext g_ucx_ctx;

// Mock NPU device memory management
struct MockMemoryManager {
    std::unordered_map<void*, size_t> allocations;
    std::mutex mutex;
};

static MockMemoryManager g_mem_manager;

// Helper function to check UCX status
static HcclResult CheckUCXStatus(ucs_status_t status, const char* operation) {
    if (status != UCS_OK) {
        std::cerr << "UCX error in " << operation << ": "
                  << ucs_status_string(status) << std::endl;
        return HCCL_E_INTERNAL;
    }
    return HCCL_SUCCESS;
}

HcclResult P2PMockInit(bool useRDMA) {
    std::lock_guard<std::mutex> lock(g_ucx_ctx.mutex);

    if (g_ucx_ctx.initialized) {
        return HCCL_SUCCESS;
    }

    // Configure UCP parameters
    ucp_params_t ucp_params;
    ucp_config_t* config;
    ucs_status_t status;

    // Read UCX configuration
    status = ucp_config_read(nullptr, nullptr, &config);
    if (status != UCS_OK) {
        return CheckUCXStatus(status, "ucp_config_read");
    }

    // Set transport preference based on RDMA flag
    if (useRDMA) {
        ucp_config_modify(config, "TLS", "rc_x,rc_v,rc_mlx5,dc_mlx5,ud_x");
    } else {
        ucp_config_modify(config, "TLS", "tcp");
    }

    // Initialize UCP context
    std::memset(&ucp_params, 0, sizeof(ucp_params));
    ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES |
                            UCP_PARAM_FIELD_REQUEST_SIZE |
                            UCP_PARAM_FIELD_REQUEST_INIT;
    ucp_params.features = UCP_FEATURE_RMA |  // Remote Memory Access
                          UCP_FEATURE_AM;     // Active Messages
    ucp_params.request_size = 0;
    ucp_params.request_init = nullptr;

    status = ucp_init(&ucp_params, config, &g_ucx_ctx.context);
    ucp_config_release(config);
    if (status != UCS_OK) {
        return CheckUCXStatus(status, "ucp_init");
    }

    // Create UCP worker
    ucp_worker_params_t worker_params;
    std::memset(&worker_params, 0, sizeof(worker_params));
    worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_MULTI;

    status = ucp_worker_create(g_ucx_ctx.context, &worker_params, &g_ucx_ctx.worker);
    if (status != UCS_OK) {
        ucp_cleanup(g_ucx_ctx.context);
        return CheckUCXStatus(status, "ucp_worker_create");
    }

    g_ucx_ctx.initialized = true;
    return HCCL_SUCCESS;
}

HcclResult P2PMockCleanup() {
    std::lock_guard<std::mutex> lock(g_ucx_ctx.mutex);

    if (!g_ucx_ctx.initialized) {
        return HCCL_SUCCESS;
    }

    if (g_ucx_ctx.worker != nullptr) {
        ucp_worker_destroy(g_ucx_ctx.worker);
        g_ucx_ctx.worker = nullptr;
    }

    if (g_ucx_ctx.context != nullptr) {
        ucp_cleanup(g_ucx_ctx.context);
        g_ucx_ctx.context = nullptr;
    }

    g_ucx_ctx.initialized = false;
    return HCCL_SUCCESS;
}

HcclResult P2PGetRootInfo(HcclRootInfo* rootInfo) {
    if (rootInfo == nullptr) {
        return HCCL_E_PTR;
    }

    std::lock_guard<std::mutex> lock(g_ucx_ctx.mutex);

    if (!g_ucx_ctx.initialized || g_ucx_ctx.worker == nullptr) {
        return HCCL_E_UNAVAIL;
    }

    // Get worker address
    ucp_address_t* address;
    size_t address_length;
    ucs_status_t status = ucp_worker_get_address(g_ucx_ctx.worker, &address, &address_length);
    if (status != UCS_OK) {
        return CheckUCXStatus(status, "ucp_worker_get_address");
    }

    if (address_length > HCCL_ROOT_INFO_BYTES) {
        ucp_worker_release_address(g_ucx_ctx.worker, address);
        std::cerr << "Worker address too large: " << address_length
                  << " > " << HCCL_ROOT_INFO_BYTES << std::endl;
        return HCCL_E_MEMORY;
    }

    // Store address in HcclRootInfo
    std::memset(rootInfo->data, 0, HCCL_ROOT_INFO_BYTES);
    std::memcpy(rootInfo->data, address, address_length);

    ucp_worker_release_address(g_ucx_ctx.worker, address);
    return HCCL_SUCCESS;
}

HcclResult P2PCommInitRootInfo(P2PComm* comm, const HcclRootInfo* rootInfo, uint32_t rank) {
    if (comm == nullptr || rootInfo == nullptr) {
        return HCCL_E_PTR;
    }

    std::lock_guard<std::mutex> lock(g_ucx_ctx.mutex);

    if (!g_ucx_ctx.initialized || g_ucx_ctx.worker == nullptr) {
        return HCCL_E_UNAVAIL;
    }

    // Create endpoint to remote worker
    ucp_ep_params_t ep_params;
    std::memset(&ep_params, 0, sizeof(ep_params));
    ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
    ep_params.address = reinterpret_cast<const ucp_address_t*>(rootInfo->data);

    ucs_status_t status = ucp_ep_create(g_ucx_ctx.worker, &ep_params, &comm->ep);
    if (status != UCS_OK) {
        return CheckUCXStatus(status, "ucp_ep_create");
    }

    comm->rank = rank;
    comm->user_data = nullptr;

    return HCCL_SUCCESS;
}

// Completion callback for non-blocking operations
static void request_completion_callback(void* request, ucs_status_t status, void* user_data) {
    (void)request;
    (void)status;
    (void)user_data;
    // Nothing to do, request will be freed by caller
}

HcclResult P2PSend(P2PComm comm, const void* sendBuf, size_t sendBytes, uint32_t destRank) {
    (void)destRank;  // Rank is embedded in the endpoint
    if (sendBuf == nullptr || sendBytes == 0) {
        return HCCL_E_PARA;
    }

    if (comm.ep == nullptr) {
        return HCCL_E_PTR;
    }

    // Use UCX Active Messages for send
    ucp_request_param_t params;
    std::memset(&params, 0, sizeof(params));
    params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK;
    params.cb.send = request_completion_callback;

    ucs_status_ptr_t request = ucp_am_send_nbx(
        comm.ep,
        0,  // AM ID
        nullptr, 0,  // Header
        sendBuf, sendBytes,
        &params
    );

    if (request == nullptr) {
        // Operation completed immediately
        return HCCL_SUCCESS;
    } else if (UCS_PTR_IS_ERR(request)) {
        return CheckUCXStatus(UCS_PTR_STATUS(request), "ucp_am_send_nbx");
    } else {
        // Wait for completion
        ucs_status_t status;
        do {
            ucp_worker_progress(g_ucx_ctx.worker);
            status = ucp_request_check_status(request);
        } while (status == UCS_INPROGRESS);

        ucp_request_free(request);
        return CheckUCXStatus(status, "P2PSend wait");
    }
}

// Active Message receive handler
static ucs_status_t am_recv_callback(void* arg, const void* header, size_t header_length,
                                     void* data, size_t length,
                                     const ucp_am_recv_param_t* param) {
    (void)header;
    (void)header_length;
    (void)param;
    // Copy received data to user buffer
    void* user_buf = arg;
    if (user_buf != nullptr && data != nullptr) {
        std::memcpy(user_buf, data, length);
    }
    return UCS_OK;
}

HcclResult P2PRecv(P2PComm comm, void* recvBuf, size_t recvBytes, uint32_t srcRank,
                   aclrtStream stream) {
    (void)srcRank;   // Rank is embedded in the endpoint
    (void)stream;    // Synchronous implementation, stream not used
    if (recvBuf == nullptr || recvBytes == 0) {
        return HCCL_E_PARA;
    }

    if (comm.ep == nullptr) {
        return HCCL_E_PTR;
    }

    // Set AM handler for receiving
    ucp_am_handler_param_t am_params;
    std::memset(&am_params, 0, sizeof(am_params));
    am_params.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID |
                           UCP_AM_HANDLER_PARAM_FIELD_CB |
                           UCP_AM_HANDLER_PARAM_FIELD_ARG;
    am_params.id = 0;
    am_params.cb = am_recv_callback;
    am_params.arg = recvBuf;

    ucs_status_t status = ucp_worker_set_am_recv_handler(g_ucx_ctx.worker, &am_params);
    if (status != UCS_OK) {
        return CheckUCXStatus(status, "ucp_worker_set_am_recv_handler");
    }

    // Progress worker to receive messages
    size_t received = 0;
    size_t max_iterations = 1000000;
    for (size_t i = 0; i < max_iterations && received < recvBytes; ++i) {
        ucp_worker_progress(g_ucx_ctx.worker);
        // In real implementation, we'd check if data arrived
        // For now, assume data arrives after some progress calls
    }

    return HCCL_SUCCESS;
}

HcclResult P2PCommDestroy(P2PComm* comm) {
    if (comm == nullptr) {
        return HCCL_E_PTR;
    }

    if (comm->ep != nullptr) {
        ucp_request_param_t params;
        std::memset(&params, 0, sizeof(params));
        params.op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS;
        params.flags = UCP_EP_CLOSE_FLAG_FORCE;

        ucs_status_ptr_t request = ucp_ep_close_nbx(comm->ep, &params);

        if (request != nullptr && !UCS_PTR_IS_ERR(request)) {
            ucs_status_t status;
            do {
                ucp_worker_progress(g_ucx_ctx.worker);
                status = ucp_request_check_status(request);
            } while (status == UCS_INPROGRESS);
            ucp_request_free(request);
        }

        comm->ep = nullptr;
    }

    comm->rank = 0;
    comm->user_data = nullptr;

    return HCCL_SUCCESS;
}

// Mock NPU memory management functions
void* MockDeviceMalloc(size_t size) {
    if (size == 0) {
        return nullptr;
    }

    // Allocate aligned host memory to simulate device memory
    void* ptr = nullptr;
    if (posix_memalign(&ptr, 512, size) != 0) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(g_mem_manager.mutex);
    g_mem_manager.allocations[ptr] = size;

    return ptr;
}

void MockDeviceFree(void* ptr) {
    if (ptr == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mem_manager.mutex);
    auto it = g_mem_manager.allocations.find(ptr);
    if (it != g_mem_manager.allocations.end()) {
        g_mem_manager.allocations.erase(it);
        free(ptr);
    }
}

void MockMemcpyH2D(void* dst, const void* src, size_t size) {
    if (dst == nullptr || src == nullptr || size == 0) {
        return;
    }
    std::memcpy(dst, src, size);
}

void MockMemcpyD2H(void* dst, const void* src, size_t size) {
    if (dst == nullptr || src == nullptr || size == 0) {
        return;
    }
    std::memcpy(dst, src, size);
}

void MockMemcpyD2D(void* dst, const void* src, size_t size) {
    if (dst == nullptr || src == nullptr || size == 0) {
        return;
    }
    std::memcpy(dst, src, size);
}

}  // namespace zerokv
