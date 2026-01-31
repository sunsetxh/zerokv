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
 * @file p2p_ucx_mock.h
 * @brief P2P-Transfer API mock implementation using UCX
 *
 * This file provides a mock implementation of P2P-Transfer APIs using UCX,
 * allowing development and testing without NPU hardware.
 */

#ifndef ZEROKV_COMMON_P2P_UCX_MOCK_H
#define ZEROKV_COMMON_P2P_UCX_MOCK_H

#include <cstdint>
#include <cstddef>

// Forward declarations for UCX types
typedef struct ucp_context* ucp_context_h;
typedef struct ucp_worker* ucp_worker_h;
typedef struct ucp_ep* ucp_ep_h;
typedef struct ucp_address ucp_address_t;

namespace zerokv {

// Mock P2P types (mimicking HCCL types)
typedef void* aclrtStream;

enum HcclResult {
    HCCL_SUCCESS = 0,
    HCCL_E_PARA = 1,
    HCCL_E_PTR = 2,
    HCCL_E_MEMORY = 3,
    HCCL_E_INTERNAL = 4,
    HCCL_E_NOT_SUPPORT = 5,
    HCCL_E_NOT_FOUND = 6,
    HCCL_E_UNAVAIL = 7,
    HCCL_E_SYSCALL = 8,
    HCCL_E_TIMEOUT = 9,
    HCCL_E_OPEN_FILE_FAILURE = 10,
    HCCL_E_TCP_CONNECT = 11,
    HCCL_E_ROCE_CONNECT = 12,
    HCCL_E_TCP_TRANSFER = 13,
    HCCL_E_ROCE_TRANSFER = 14,
    HCCL_E_RUNTIME = 15,
    HCCL_E_DRV = 16,
    HCCL_E_PROFILING = 17,
    HCCL_E_CCE = 18,
    HCCL_E_NETWORK = 19,
    HCCL_E_RESERVED = 255
};

// HcclRootInfo structure (stores UCX worker address)
constexpr size_t HCCL_ROOT_INFO_BYTES = 4096;
struct HcclRootInfo {
    uint8_t data[HCCL_ROOT_INFO_BYTES];
};

// P2PComm handle (wraps UCX endpoint)
struct P2PComm {
    ucp_ep_h ep;
    uint32_t rank;
    void* user_data;
};

/**
 * @brief Initialize P2P Mock environment
 *
 * @param useRDMA Whether to use RDMA transport (true) or TCP (false)
 * @return HCCL_SUCCESS on success, error code otherwise
 */
HcclResult P2PMockInit(bool useRDMA = true);

/**
 * @brief Cleanup P2P Mock environment
 *
 * @return HCCL_SUCCESS on success, error code otherwise
 */
HcclResult P2PMockCleanup();

/**
 * @brief Get local P2P root information
 *
 * This function retrieves the local worker's connection information
 * and stores it in HcclRootInfo structure.
 *
 * @param rootInfo Output buffer for root information
 * @return HCCL_SUCCESS on success, error code otherwise
 */
HcclResult P2PGetRootInfo(HcclRootInfo* rootInfo);

/**
 * @brief Initialize P2P communication with remote root info
 *
 * @param comm Output communication handle
 * @param rootInfo Remote worker's root information
 * @param rank Remote rank/device ID
 * @return HCCL_SUCCESS on success, error code otherwise
 */
HcclResult P2PCommInitRootInfo(P2PComm* comm, const HcclRootInfo* rootInfo, uint32_t rank);

/**
 * @brief Send data to remote peer
 *
 * @param comm Communication handle
 * @param sendBuf Local buffer to send
 * @param sendBytes Number of bytes to send
 * @param destRank Destination rank
 * @return HCCL_SUCCESS on success, error code otherwise
 */
HcclResult P2PSend(P2PComm comm, const void* sendBuf, size_t sendBytes, uint32_t destRank);

/**
 * @brief Receive data from remote peer
 *
 * @param comm Communication handle
 * @param recvBuf Local buffer to receive into
 * @param recvBytes Number of bytes to receive
 * @param srcRank Source rank
 * @param stream ACL stream for async operation (can be nullptr for sync)
 * @return HCCL_SUCCESS on success, error code otherwise
 */
HcclResult P2PRecv(P2PComm comm, void* recvBuf, size_t recvBytes, uint32_t srcRank,
                   aclrtStream stream = nullptr);

/**
 * @brief Destroy P2P communication handle
 *
 * @param comm Communication handle to destroy
 * @return HCCL_SUCCESS on success, error code otherwise
 */
HcclResult P2PCommDestroy(P2PComm* comm);

// Mock NPU memory management functions
void* MockDeviceMalloc(size_t size);
void MockDeviceFree(void* ptr);
void MockMemcpyH2D(void* dst, const void* src, size_t size);
void MockMemcpyD2H(void* dst, const void* src, size_t size);
void MockMemcpyD2D(void* dst, const void* src, size_t size);

}  // namespace zerokv

#endif  // ZEROKV_COMMON_P2P_UCX_MOCK_H
