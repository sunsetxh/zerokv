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
 * @file ucx_stub.h
 * @brief Minimal UCX API stub for development without UCX installation
 *
 * This file provides stub definitions for UCX APIs to allow compilation
 * and basic testing without a full UCX installation. It is NOT suitable
 * for production use - real UCX should be used in production environments.
 */

#ifndef ZEROKV_COMMON_UCX_STUB_H
#define ZEROKV_COMMON_UCX_STUB_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

// UCX status codes
typedef enum {
    UCS_OK                  =  0,
    UCS_INPROGRESS          = -1,
    UCS_ERR_NO_MEMORY       = -2,
    UCS_ERR_INVALID_PARAM   = -3,
    UCS_ERR_NO_RESOURCE     = -4,
    UCS_ERR_IO_ERROR        = -5,
    UCS_ERR_NOT_IMPLEMENTED = -6,
} ucs_status_t;

// UCX thread modes
typedef enum {
    UCS_THREAD_MODE_SINGLE,
    UCS_THREAD_MODE_SERIALIZED,
    UCS_THREAD_MODE_MULTI
} ucs_thread_mode_t;

// Opaque handles
typedef struct ucp_context* ucp_context_h;
typedef struct ucp_worker* ucp_worker_h;
typedef struct ucp_ep* ucp_ep_h;
typedef struct ucp_listener* ucp_listener_h;
typedef struct ucp_conn_request* ucp_conn_request_h;
typedef struct ucp_address ucp_address_t;
typedef struct ucp_config ucp_config_t;
typedef void* ucs_status_ptr_t;

// Field masks for parameter structures
#define UCP_PARAM_FIELD_FEATURES        (1ULL << 0)
#define UCP_PARAM_FIELD_REQUEST_SIZE    (1ULL << 1)
#define UCP_PARAM_FIELD_REQUEST_INIT    (1ULL << 2)
#define UCP_WORKER_PARAM_FIELD_THREAD_MODE (1ULL << 0)
#define UCP_EP_PARAM_FIELD_REMOTE_ADDRESS (1ULL << 0)
#define UCP_EP_PARAM_FIELD_CONN_REQUEST  (1ULL << 1)
#define UCP_OP_ATTR_FIELD_CALLBACK      (1ULL << 0)
#define UCP_OP_ATTR_FIELD_FLAGS         (1ULL << 1)
#define UCP_AM_HANDLER_PARAM_FIELD_ID   (1ULL << 0)
#define UCP_AM_HANDLER_PARAM_FIELD_CB   (1ULL << 1)
#define UCP_AM_HANDLER_PARAM_FIELD_ARG  (1ULL << 2)
#define UCP_LISTENER_PARAM_FIELD_SOCK_ADDR (1ULL << 0)
#define UCP_LISTENER_PARAM_FIELD_CONN_HANDLER (1ULL << 1)

// Features
#define UCP_FEATURE_RMA     (1ULL << 0)
#define UCP_FEATURE_AM      (1ULL << 1)
#define UCP_FEATURE_STREAM  (1ULL << 2)
#define UCP_FEATURE_TAG     (1ULL << 3)

// Flags
#define UCP_EP_CLOSE_FLAG_FORCE (1ULL << 0)

// Check if status pointer is an error
#define UCS_PTR_IS_ERR(_ptr) \
    (((uintptr_t)(_ptr)) >= ((uintptr_t)UCS_ERR_NOT_IMPLEMENTED))

#define UCS_PTR_STATUS(_ptr) \
    ((ucs_status_t)(intptr_t)(_ptr))

// Parameter structures
typedef struct {
    uint64_t field_mask;
    uint64_t features;
    size_t request_size;
    void (*request_init)(void* request);
} ucp_params_t;

typedef struct {
    uint64_t field_mask;
    ucs_thread_mode_t thread_mode;
} ucp_worker_params_t;

// EP parameter field masks (add missing ones, keep existing)
#define UCP_EP_PARAM_FIELD_REMOTE_ADDRESS       (1ULL << 0)
#define UCP_EP_PARAM_FIELD_FLAGS                (1ULL << 3)
#define UCP_EP_PARAM_FIELD_SOCK_ADDR            (1ULL << 4)
#define UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE    (1ULL << 5)

// EP flags
#define UCP_EP_PARAMS_FLAGS_CLIENT_SERVER       (1ULL << 0)

// Error handling modes
#define UCP_ERR_HANDLING_MODE_NONE              0
#define UCP_ERR_HANDLING_MODE_PEER              1

// Socket address (UCS type) - defined here before use
typedef struct ucs_sock_addr {
    const struct sockaddr* addr;
    socklen_t addrlen;
} ucs_sock_addr_t;

typedef struct {
    uint64_t field_mask;
    const ucp_address_t* address;
    ucp_conn_request_h conn_request;
    uint32_t flags;
    ucs_sock_addr_t sockaddr;
    uint32_t err_mode;
} ucp_ep_params_t;

// Connection request callback
typedef void (*ucp_connection_request_callback_t)(ucp_conn_request_h conn_request, void* arg);

// Listener connection handler structure
typedef struct {
    ucp_connection_request_callback_t cb;
    void* arg;
} ucp_listener_conn_handler_t;

typedef struct {
    uint64_t field_mask;
    ucs_sock_addr_t sockaddr;
    ucp_listener_conn_handler_t conn_handler;
} ucp_listener_params_t;

// Tag receive info
typedef struct {
    size_t length;
} ucp_tag_recv_info_t;

// EP close flags
#define UCP_EP_CLOSE_FLAG_FORCE     (1ULL << 0)

// UCS_PTR_IS_PTR was missing, add it
#define UCS_PTR_IS_PTR(_ptr)    ((uintptr_t)(_ptr) >= 0x100)

typedef struct {
    uint64_t op_attr_mask;
    union {
        struct {
            void (*send)(void* request, ucs_status_t status, void* user_data);
        } cb;
        void* user_data;
    };
    uint32_t flags;
} ucp_request_param_t;

typedef struct {
    int dummy;  // Placeholder to avoid empty struct warning
} ucp_am_recv_param_t;

typedef struct {
    uint64_t field_mask;
    unsigned id;
    ucs_status_t (*cb)(void* arg, const void* header, size_t header_length,
                       void* data, size_t length, const ucp_am_recv_param_t* param);
    void* arg;
} ucp_am_handler_param_t;

// Function declarations
const char* ucs_status_string(ucs_status_t status);

ucs_status_t ucp_config_read(const char* env_prefix, const char* filename,
                             ucp_config_t** config_p);
void ucp_config_release(ucp_config_t* config);
ucs_status_t ucp_config_modify(ucp_config_t* config, const char* name,
                               const char* value);

ucs_status_t ucp_init(const ucp_params_t* params, const ucp_config_t* config,
                      ucp_context_h* context_p);
void ucp_cleanup(ucp_context_h context);

ucs_status_t ucp_worker_create(ucp_context_h context,
                               const ucp_worker_params_t* params,
                               ucp_worker_h* worker_p);
void ucp_worker_destroy(ucp_worker_h worker);
unsigned ucp_worker_progress(ucp_worker_h worker);

ucs_status_t ucp_worker_get_address(ucp_worker_h worker,
                                    ucp_address_t** address_p,
                                    size_t* address_length_p);
void ucp_worker_release_address(ucp_worker_h worker, ucp_address_t* address);

ucs_status_t ucp_ep_create(ucp_worker_h worker, const ucp_ep_params_t* params,
                           ucp_ep_h* ep_p);
ucs_status_ptr_t ucp_ep_close_nbx(ucp_ep_h ep, const ucp_request_param_t* param);

ucs_status_t ucp_listener_create(ucp_worker_h worker, const ucp_listener_params_t* params,
                                ucp_listener_h* listener_p);
void ucp_listener_destroy(ucp_listener_h listener);
void ucp_listener_reject(ucp_listener_h listener, ucp_conn_request_h conn_request);

ucs_status_ptr_t ucp_am_send_nbx(ucp_ep_h ep, unsigned id,
                                  const void* header, size_t header_length,
                                  const void* buffer, size_t count,
                                  const ucp_request_param_t* param);

ucs_status_t ucp_worker_set_am_recv_handler(ucp_worker_h worker,
                                             const ucp_am_handler_param_t* param);

// Stream API
ucs_status_ptr_t ucp_stream_send_nbx(ucp_ep_h ep, const void* buffer, size_t count,
                                      const ucp_request_param_t* param);
ucs_status_ptr_t ucp_stream_recv_nbx(ucp_ep_h ep, void* buffer, size_t count,
                                      size_t* length, const ucp_request_param_t* param);

ucs_status_t ucp_request_check_status(void* request);
void ucp_request_free(void* request);

#ifdef __cplusplus
}
#endif

#endif  // ZEROKV_COMMON_UCX_STUB_H
