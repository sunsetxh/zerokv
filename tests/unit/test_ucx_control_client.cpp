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
    UCXControlClient client;
    EXPECT_TRUE(client.Initialize());

    // Try to connect to a port that's likely not in use
    EXPECT_FALSE(client.Connect("127.0.0.1", 19999));
}

// Test: Connect and disconnect
TEST_F(UCXControlClientTest, ConnectDisconnectTest) {
    // Start a server
    UCXServerConfig server_config;
    server_config.listen_port = 18516;
    UCXControlServer server(server_config);

    ASSERT_TRUE(server.Initialize());
    ASSERT_TRUE(server.Start());

    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Connect client
    UCXControlClient client;
    EXPECT_TRUE(client.Initialize());
    EXPECT_TRUE(client.Connect("127.0.0.1", 18516));
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
    server_config.listen_port = 18517;
    UCXControlServer server(server_config);

    ASSERT_TRUE(server.Initialize());
    ASSERT_TRUE(server.Start());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    UCXControlClient client;
    EXPECT_TRUE(client.Initialize());
    EXPECT_TRUE(client.Connect("127.0.0.1", 18517));
    EXPECT_TRUE(client.Connect("127.0.0.1", 18517));  // Should succeed (idempotent)

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
    server_config.listen_port = 18518;
    UCXControlServer server(server_config);

    ASSERT_TRUE(server.Initialize());
    ASSERT_TRUE(server.Start());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    UCXControlClient client;
    EXPECT_TRUE(client.Initialize());
    EXPECT_TRUE(client.Connect("127.0.0.1", 18518));

    EXPECT_EQ(client.GetServerAddress(), "127.0.0.1");
    EXPECT_EQ(client.GetServerPort(), 18518);

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
    server_config.listen_port = 18519;
    server_config.max_connections = 10;
    UCXControlServer server(server_config);

    ASSERT_TRUE(server.Initialize());
    ASSERT_TRUE(server.Start());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Create multiple clients
    std::vector<std::unique_ptr<UCXControlClient>> clients;
    for (int i = 0; i < 5; ++i) {
        auto client = std::make_unique<UCXControlClient>();
        EXPECT_TRUE(client->Initialize());
        EXPECT_TRUE(client->Connect("127.0.0.1", 18519));
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
