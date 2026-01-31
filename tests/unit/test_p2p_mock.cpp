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
 * @file test_p2p_mock.cpp
 * @brief Unit tests for P2P UCX Mock implementation
 */

#include "../../src/common/p2p_ucx_mock.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include <thread>

using namespace zerokv;

class P2PMockTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize P2P Mock for each test
        HcclResult result = P2PMockInit(false);  // Use TCP for tests
        ASSERT_EQ(result, HCCL_SUCCESS);
    }

    void TearDown() override {
        // Cleanup after each test
        HcclResult result = P2PMockCleanup();
        ASSERT_EQ(result, HCCL_SUCCESS);
    }
};

// Test: P2P Mock initialization
TEST_F(P2PMockTest, InitializationTest) {
    // Already initialized in SetUp, just verify it works
    EXPECT_EQ(HCCL_SUCCESS, HCCL_SUCCESS);
}

// Test: Double initialization should succeed
TEST_F(P2PMockTest, DoubleInitTest) {
    HcclResult result = P2PMockInit(false);
    EXPECT_EQ(result, HCCL_SUCCESS);
}

// Test: Get root info
TEST_F(P2PMockTest, GetRootInfoTest) {
    HcclRootInfo rootInfo;
    HcclResult result = P2PGetRootInfo(&rootInfo);
    EXPECT_EQ(result, HCCL_SUCCESS);

    // Verify that some data was written (UCX worker address should be non-zero)
    bool hasData = false;
    for (size_t i = 0; i < HCCL_ROOT_INFO_BYTES; ++i) {
        if (rootInfo.data[i] != 0) {
            hasData = true;
            break;
        }
    }
    EXPECT_TRUE(hasData);
}

// Test: Get root info with null pointer
TEST_F(P2PMockTest, GetRootInfoNullTest) {
    HcclResult result = P2PGetRootInfo(nullptr);
    EXPECT_EQ(result, HCCL_E_PTR);
}

// Test: Comm initialization
TEST_F(P2PMockTest, CommInitTest) {
    // Get root info
    HcclRootInfo rootInfo;
    ASSERT_EQ(P2PGetRootInfo(&rootInfo), HCCL_SUCCESS);

    // Initialize communication with self
    P2PComm comm;
    HcclResult result = P2PCommInitRootInfo(&comm, &rootInfo, 0);
    EXPECT_EQ(result, HCCL_SUCCESS);
    EXPECT_NE(comm.ep, nullptr);
    EXPECT_EQ(comm.rank, 0u);

    // Cleanup
    EXPECT_EQ(P2PCommDestroy(&comm), HCCL_SUCCESS);
}

// Test: Comm initialization with null pointers
TEST_F(P2PMockTest, CommInitNullTest) {
    HcclRootInfo rootInfo = {};
    P2PComm comm;

    EXPECT_EQ(P2PCommInitRootInfo(nullptr, &rootInfo, 0), HCCL_E_PTR);
    EXPECT_EQ(P2PCommInitRootInfo(&comm, nullptr, 0), HCCL_E_PTR);
}

// Test: Mock device memory allocation
TEST_F(P2PMockTest, MockMemoryAllocTest) {
    const size_t size = 1024;
    void* ptr = MockDeviceMalloc(size);
    EXPECT_NE(ptr, nullptr);

    // Verify memory is usable
    std::memset(ptr, 0xAB, size);

    MockDeviceFree(ptr);
}

// Test: Mock device memory - zero size
TEST_F(P2PMockTest, MockMemoryZeroSizeTest) {
    void* ptr = MockDeviceMalloc(0);
    EXPECT_EQ(ptr, nullptr);
}

// Test: Mock device memory - free null pointer
TEST_F(P2PMockTest, MockMemoryFreeNullTest) {
    // Should not crash
    MockDeviceFree(nullptr);
}

// Test: Mock memory copy H2D
TEST_F(P2PMockTest, MockMemcpyH2DTest) {
    const size_t size = 256;
    std::vector<uint8_t> hostData(size, 0x42);
    void* devPtr = MockDeviceMalloc(size);
    ASSERT_NE(devPtr, nullptr);

    MockMemcpyH2D(devPtr, hostData.data(), size);

    // Verify by copying back
    std::vector<uint8_t> verifyData(size, 0);
    MockMemcpyD2H(verifyData.data(), devPtr, size);

    EXPECT_EQ(hostData, verifyData);

    MockDeviceFree(devPtr);
}

// Test: Mock memory copy D2D
TEST_F(P2PMockTest, MockMemcpyD2DTest) {
    const size_t size = 128;
    std::vector<uint8_t> testData(size, 0x55);

    void* devPtr1 = MockDeviceMalloc(size);
    void* devPtr2 = MockDeviceMalloc(size);
    ASSERT_NE(devPtr1, nullptr);
    ASSERT_NE(devPtr2, nullptr);

    MockMemcpyH2D(devPtr1, testData.data(), size);
    MockMemcpyD2D(devPtr2, devPtr1, size);

    std::vector<uint8_t> verifyData(size, 0);
    MockMemcpyD2H(verifyData.data(), devPtr2, size);

    EXPECT_EQ(testData, verifyData);

    MockDeviceFree(devPtr1);
    MockDeviceFree(devPtr2);
}

// Test: P2P Send/Recv with loopback
TEST_F(P2PMockTest, SendRecvLoopbackTest) {
    // Get root info
    HcclRootInfo rootInfo;
    ASSERT_EQ(P2PGetRootInfo(&rootInfo), HCCL_SUCCESS);

    // Initialize communication
    P2PComm comm;
    ASSERT_EQ(P2PCommInitRootInfo(&comm, &rootInfo, 0), HCCL_SUCCESS);

    // Prepare test data
    const size_t dataSize = 64;
    std::vector<uint8_t> sendData(dataSize);
    for (size_t i = 0; i < dataSize; ++i) {
        sendData[i] = static_cast<uint8_t>(i);
    }
    std::vector<uint8_t> recvData(dataSize, 0);

    // Launch receiver in separate thread
    std::thread recvThread([&]() {
        HcclResult result = P2PRecv(comm, recvData.data(), dataSize, 0, nullptr);
        EXPECT_EQ(result, HCCL_SUCCESS);
    });

    // Give receiver time to set up
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send data
    HcclResult result = P2PSend(comm, sendData.data(), dataSize, 0);
    EXPECT_EQ(result, HCCL_SUCCESS);

    // Wait for receiver
    recvThread.join();

    // Cleanup
    EXPECT_EQ(P2PCommDestroy(&comm), HCCL_SUCCESS);
}

// Test: P2P Send with invalid parameters
TEST_F(P2PMockTest, SendInvalidParamsTest) {
    HcclRootInfo rootInfo;
    ASSERT_EQ(P2PGetRootInfo(&rootInfo), HCCL_SUCCESS);

    P2PComm comm;
    ASSERT_EQ(P2PCommInitRootInfo(&comm, &rootInfo, 0), HCCL_SUCCESS);

    const size_t dataSize = 64;
    std::vector<uint8_t> sendData(dataSize, 0x11);

    // Null buffer
    EXPECT_EQ(P2PSend(comm, nullptr, dataSize, 0), HCCL_E_PARA);

    // Zero size
    EXPECT_EQ(P2PSend(comm, sendData.data(), 0, 0), HCCL_E_PARA);

    // Null endpoint
    P2PComm invalidComm = {nullptr, 0, nullptr};
    EXPECT_EQ(P2PSend(invalidComm, sendData.data(), dataSize, 0), HCCL_E_PTR);

    EXPECT_EQ(P2PCommDestroy(&comm), HCCL_SUCCESS);
}

// Test: P2P Recv with invalid parameters
TEST_F(P2PMockTest, RecvInvalidParamsTest) {
    HcclRootInfo rootInfo;
    ASSERT_EQ(P2PGetRootInfo(&rootInfo), HCCL_SUCCESS);

    P2PComm comm;
    ASSERT_EQ(P2PCommInitRootInfo(&comm, &rootInfo, 0), HCCL_SUCCESS);

    const size_t dataSize = 64;
    std::vector<uint8_t> recvData(dataSize, 0);

    // Null buffer
    EXPECT_EQ(P2PRecv(comm, nullptr, dataSize, 0, nullptr), HCCL_E_PARA);

    // Zero size
    EXPECT_EQ(P2PRecv(comm, recvData.data(), 0, 0, nullptr), HCCL_E_PARA);

    // Null endpoint
    P2PComm invalidComm = {nullptr, 0, nullptr};
    EXPECT_EQ(P2PRecv(invalidComm, recvData.data(), dataSize, 0, nullptr), HCCL_E_PTR);

    EXPECT_EQ(P2PCommDestroy(&comm), HCCL_SUCCESS);
}

// Test: Multiple allocations and frees
TEST_F(P2PMockTest, MultipleAllocationsTest) {
    const int numAllocs = 10;
    const size_t size = 512;
    std::vector<void*> ptrs;

    // Allocate multiple buffers
    for (int i = 0; i < numAllocs; ++i) {
        void* ptr = MockDeviceMalloc(size);
        EXPECT_NE(ptr, nullptr);
        ptrs.push_back(ptr);
    }

    // Free all buffers
    for (void* ptr : ptrs) {
        MockDeviceFree(ptr);
    }
}

// Test: Large allocation
TEST_F(P2PMockTest, LargeAllocationTest) {
    const size_t largeSize = 10 * 1024 * 1024;  // 10MB
    void* ptr = MockDeviceMalloc(largeSize);
    EXPECT_NE(ptr, nullptr);

    if (ptr != nullptr) {
        // Write pattern
        uint8_t* bytePtr = static_cast<uint8_t*>(ptr);
        for (size_t i = 0; i < 1024; ++i) {
            bytePtr[i] = static_cast<uint8_t>(i % 256);
        }

        // Verify pattern
        for (size_t i = 0; i < 1024; ++i) {
            EXPECT_EQ(bytePtr[i], static_cast<uint8_t>(i % 256));
        }

        MockDeviceFree(ptr);
    }
}

// Test: Comm destroy with null
TEST_F(P2PMockTest, CommDestroyNullTest) {
    EXPECT_EQ(P2PCommDestroy(nullptr), HCCL_E_PTR);
}

// Test: RDMA mode initialization
TEST_F(P2PMockTest, RDMAModeTest) {
    P2PMockCleanup();
    HcclResult result = P2PMockInit(true);  // Use RDMA

    // Skip test if RDMA devices are not available
    if (result != HCCL_SUCCESS) {
        GTEST_SKIP() << "RDMA devices not available, skipping RDMA test";
    }

    EXPECT_EQ(result, HCCL_SUCCESS);

    HcclRootInfo rootInfo;
    EXPECT_EQ(P2PGetRootInfo(&rootInfo), HCCL_SUCCESS);

    P2PMockCleanup();
    // Re-init with TCP for other tests
    P2PMockInit(false);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
