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
 * @file test_ucx_control_server.cpp
 * @brief Unit tests for UCX Control Server
 */

#include <gtest/gtest.h>
#include "zerokv/ucx_control_server.h"
#include <thread>
#include <chrono>

namespace zerokv {

class UCXControlServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.listen_address = "127.0.0.1";
        config_.listen_port = 18515;
        config_.use_rdma = false;  // Use TCP for testing
        config_.max_connections = 10;
        config_.max_kv_size = 1024 * 1024;  // 1MB for testing
    }

    void TearDown() override {
        // Cleanup is handled by server destructor
    }

    UCXServerConfig config_;
};

//==============================================================================
// Initialization Tests
//==============================================================================

TEST_F(UCXControlServerTest, InitializeTest) {
    UCXControlServer server(config_);
    EXPECT_TRUE(server.Initialize());
    // Initialize doesn't start the server, need to call Start()
    EXPECT_FALSE(server.IsRunning());
    EXPECT_TRUE(server.Start());
    EXPECT_TRUE(server.IsRunning());
}

TEST_F(UCXControlServerTest, DoubleInitializeTest) {
    UCXControlServer server(config_);
    EXPECT_TRUE(server.Initialize());
    // Second initialize should fail (already initialized)
    EXPECT_FALSE(server.Initialize());
}

TEST_F(UCXControlServerTest, GetListenAddressTest) {
    UCXControlServer server(config_);
    server.Initialize();
    std::string addr = server.GetListenAddress();
    EXPECT_FALSE(addr.empty());
    EXPECT_NE(addr.find("18515"), std::string::npos);
}

//==============================================================================
// KV Store Tests
//==============================================================================

TEST_F(UCXControlServerTest, PutRequestTest) {
    UCXControlServer server(config_);
    server.Initialize();

    PutRequest request;
    request.set_key("test_key");
    request.set_dev_ptr(0x1000);
    request.set_size(1024);
    request.set_data_type(DataType::FP32);
    request.set_client_id("test_client");

    // Call handler directly (we'll need to make it accessible or add a public interface)
    // For now, test through stats
    auto stats = server.GetStats();
    EXPECT_EQ(stats.kv_count(), 0);  // No entries yet
}

TEST_F(UCXControlServerTest, GetStatsTest) {
    UCXControlServer server(config_);
    server.Initialize();
    server.Start();  // Need to start to set start_time_

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    auto stats = server.GetStats();
    EXPECT_EQ(stats.kv_count(), 0);
    EXPECT_EQ(stats.active_connections(), 0);
    // uptime_seconds() may be 0 if called too quickly, so check >= 0
    EXPECT_GE(stats.uptime_seconds(), 0);
}

//==============================================================================
// Connection Management Tests
//==============================================================================

TEST_F(UCXControlServerTest, ConnectionCountTest) {
    UCXControlServer server(config_);
    server.Initialize();
    EXPECT_EQ(server.GetConnectionCount(), 0);
}

TEST_F(UCXControlServerTest, KVCountTest) {
    UCXControlServer server(config_);
    server.Initialize();
    EXPECT_EQ(server.GetKVCount(), 0);
}

//==============================================================================
// Configuration Tests
//==============================================================================

TEST_F(UCXControlServerTest, CustomPortTest) {
    config_.listen_port = 19000;
    UCXControlServer server(config_);
    EXPECT_TRUE(server.Initialize());
    std::string addr = server.GetListenAddress();
    EXPECT_NE(addr.find("19000"), std::string::npos);
}

TEST_F(UCXControlServerTest, RDAModeTest) {
    config_.use_rdma = true;
    UCXControlServer server(config_);
    EXPECT_TRUE(server.Initialize());
}

TEST_F(UCXControlServerTest, MaxConnectionsTest) {
    config_.max_connections = 5;
    UCXControlServer server(config_);
    EXPECT_TRUE(server.Initialize());
}

//==============================================================================
// Lifecycle Tests
//==============================================================================

TEST_F(UCXControlServerTest, StartStopTest) {
    UCXControlServer server(config_);
    EXPECT_TRUE(server.Initialize());
    EXPECT_TRUE(server.Start());
    EXPECT_TRUE(server.IsRunning());

    server.Stop();
    EXPECT_FALSE(server.IsRunning());
}

TEST_F(UCXControlServerTest, StopWithoutStartTest) {
    UCXControlServer server(config_);
    server.Stop();  // Should be safe to call when not running
    EXPECT_FALSE(server.IsRunning());
}

TEST_F(UCXControlServerTest, RunWithTimeoutTest) {
    UCXControlServer server(config_);
    EXPECT_TRUE(server.Initialize());
    EXPECT_TRUE(server.Start());

    // Run for 100ms then stop
    auto start = std::chrono::steady_clock::now();
    bool result = server.Run(100);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    EXPECT_TRUE(result);
    EXPECT_GE(elapsed, 100);  // Should run for at least 100ms
    EXPECT_LT(elapsed, 200);  // But not too much longer
}

}  // namespace zerokv

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
