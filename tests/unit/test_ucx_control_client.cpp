/**
 * Copyright (c) 2025. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include <gtest/gtest.h>
#include <zerokv/ucx_control_client.h>
#include <zerokv/ucx_control_server.h>
#include <zerokv/logger.h>

#include <thread>
#include <chrono>

using namespace zerokv;

class UCXControlClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Disable logger for tests
        zerokv::LogManager::Instance().SetLevel(zerokv::LogLevel::ERROR);
    }

    void TearDown() override {
        // Add delay to allow TCP sockets to be released (TIME_WAIT state)
        // Need longer delay for TCP TIME_WAIT to clear in real UCX environment
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
};

// Test: Client initialization
TEST_F(UCXControlClientTest, InitializationTest) {
    UCXControlClient client;
    EXPECT_TRUE(client.Initialize());
    EXPECT_FALSE(client.IsConnected());
}

// Test: Double initialization
TEST_F(UCXControlClientTest, DoubleInitializeTest) {
    UCXControlClient client;
    EXPECT_TRUE(client.Initialize());
    EXPECT_TRUE(client.Initialize());  // Should succeed (idempotent)
}

// Test: Connect without initialization
TEST_F(UCXControlClientTest, ConnectWithoutInitTest) {
    UCXControlClient client;
    EXPECT_FALSE(client.Connect("127.0.0.1", 18515));
}

// Test: Connect to non-existent server
TEST_F(UCXControlClientTest, ConnectToNonExistentServerTest) {
    // Use shorter timeout and no retries for this test
    UCXClientConfig config;
    config.request_timeout_ms = 500;   // 500ms timeout
    config.max_retries = 0;            // No retries
    config.use_rdma = false;
    UCXControlClient client(config);

    EXPECT_TRUE(client.Initialize());

    // UCX uses async connection model - Connect() will succeed immediately
    // but the actual connection error will be detected during data transfer
    EXPECT_TRUE(client.Connect("127.0.0.1", 19999));
    EXPECT_TRUE(client.IsConnected());

    // Try to send a message - this should fail or timeout
    ServerStatsRequest request;
    auto result = client.GetStats(request);

    // Should fail with timeout or network error since server doesn't exist
    EXPECT_NE(result.status, RPCStatus::SUCCESS);
    EXPECT_TRUE(result.status == RPCStatus::TIMEOUT ||
                result.status == RPCStatus::NETWORK_ERROR);
}

// Test: Connect and disconnect
TEST_F(UCXControlClientTest, ConnectDisconnectTest) {
    // Start a server
    UCXServerConfig server_config;
    server_config.listen_address = "127.0.0.1";  // Use localhost
    server_config.listen_port = 19100;  // Unique port with wider spacing
    server_config.use_rdma = false;  // Force TCP transport
    UCXControlServer server(server_config);

    ASSERT_TRUE(server.Initialize());
    ASSERT_TRUE(server.Start());

    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Connect client
    UCXClientConfig client_config;
    client_config.use_rdma = false;  // Force TCP transport
    UCXControlClient client(client_config);
    EXPECT_TRUE(client.Initialize());
    EXPECT_TRUE(client.Connect("127.0.0.1", 19100));
    EXPECT_TRUE(client.IsConnected());

    // Disconnect
    client.Disconnect();
    EXPECT_FALSE(client.IsConnected());

    // Stop server
    server.Stop();
}

// Test: Double connect
TEST_F(UCXControlClientTest, DoubleConnectTest) {
    UCXServerConfig server_config;
    server_config.listen_address = "127.0.0.1";  // Use localhost
    server_config.listen_port = 19200;  // Unique port with wider spacing
    server_config.use_rdma = false;  // Force TCP transport
    UCXControlServer server(server_config);

    ASSERT_TRUE(server.Initialize());
    ASSERT_TRUE(server.Start());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    UCXClientConfig client_config;
    client_config.use_rdma = false;  // Force TCP transport
    UCXControlClient client(client_config);
    EXPECT_TRUE(client.Initialize());
    EXPECT_TRUE(client.Connect("127.0.0.1", 19200));
    EXPECT_TRUE(client.Connect("127.0.0.1", 18601));  // Should succeed (idempotent)

    client.Disconnect();
    server.Stop();
}

// Test: Put request without connection
TEST_F(UCXControlClientTest, PutWithoutConnectionTest) {
    UCXControlClient client;
    EXPECT_TRUE(client.Initialize());

    PutRequest request;
    request.set_key("test_key");
    request.set_dev_ptr(0x1000);
    request.set_size(1024);
    request.set_data_type(DataType::FP32);
    request.set_client_id("test_client");

    auto result = client.Put(request);
    EXPECT_EQ(result.status, RPCStatus::NOT_CONNECTED);
}

// Test: Get request without connection
TEST_F(UCXControlClientTest, GetWithoutConnectionTest) {
    UCXControlClient client;
    EXPECT_TRUE(client.Initialize());

    GetRequest request;
    request.set_key("test_key");
    request.set_client_id("test_client");

    auto result = client.Get(request);
    EXPECT_EQ(result.status, RPCStatus::NOT_CONNECTED);
}

// Test: Delete request without connection
TEST_F(UCXControlClientTest, DeleteWithoutConnectionTest) {
    UCXControlClient client;
    EXPECT_TRUE(client.Initialize());

    DeleteRequest request;
    request.set_key("test_key");
    request.set_client_id("test_client");

    auto result = client.Delete(request);
    EXPECT_EQ(result.status, RPCStatus::NOT_CONNECTED);
}

// Test: Stats request without connection
TEST_F(UCXControlClientTest, StatsWithoutConnectionTest) {
    UCXControlClient client;
    EXPECT_TRUE(client.Initialize());

    ServerStatsRequest request;

    auto result = client.GetStats(request);
    EXPECT_EQ(result.status, RPCStatus::NOT_CONNECTED);
}

// Test: Get server address
TEST_F(UCXControlClientTest, GetServerAddressTest) {
    UCXServerConfig server_config;
    server_config.listen_address = "127.0.0.1";  // Use localhost
    server_config.listen_port = 19300;  // Unique port with wider spacing
    server_config.use_rdma = false;  // Force TCP transport
    UCXControlServer server(server_config);

    ASSERT_TRUE(server.Initialize());
    ASSERT_TRUE(server.Start());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    UCXClientConfig client_config;
    client_config.use_rdma = false;  // Force TCP transport
    UCXControlClient client(client_config);
    EXPECT_TRUE(client.Initialize());
    EXPECT_TRUE(client.Connect("127.0.0.1", 19300));

    EXPECT_EQ(client.GetServerAddress(), "127.0.0.1");
    EXPECT_EQ(client.GetServerPort(), 19300);

    client.Disconnect();
    server.Stop();
}

// Test: Custom config
TEST_F(UCXControlClientTest, CustomConfigTest) {
    UCXClientConfig config;
    config.connect_timeout_ms = 1000;
    config.request_timeout_ms = 5000;
    config.max_retries = 5;

    UCXControlClient client(config);
    EXPECT_TRUE(client.Initialize());
}

// Test: RDMA mode
TEST_F(UCXControlClientTest, RDMAModeTest) {
    UCXClientConfig config;
    config.use_rdma = true;

    UCXControlClient client(config);
    EXPECT_TRUE(client.Initialize());
}

// Test: TCP mode
TEST_F(UCXControlClientTest, TCPModeTest) {
    UCXClientConfig config;
    config.use_rdma = false;

    UCXControlClient client(config);
    EXPECT_TRUE(client.Initialize());
}

// Test: Multiple clients
TEST_F(UCXControlClientTest, MultipleClientsTest) {
    UCXServerConfig server_config;
    server_config.listen_address = "127.0.0.1";  // Use localhost
    server_config.listen_port = 19400;  // Unique port with wider spacing
    server_config.max_connections = 10;
    server_config.use_rdma = false;  // Force TCP transport
    UCXControlServer server(server_config);

    ASSERT_TRUE(server.Initialize());
    ASSERT_TRUE(server.Start());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Create multiple clients
    UCXClientConfig client_config;
    client_config.use_rdma = false;  // Force TCP transport
    std::vector<std::unique_ptr<UCXControlClient>> clients;
    for (int i = 0; i < 5; ++i) {
        auto client = std::make_unique<UCXControlClient>(client_config);
        EXPECT_TRUE(client->Initialize());
        EXPECT_TRUE(client->Connect("127.0.0.1", 19400));
        clients.push_back(std::move(client));
    }

    // Verify all connected
    for (const auto& client : clients) {
        EXPECT_TRUE(client->IsConnected());
    }

    // Disconnect all
    clients.clear();

    server.Stop();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
