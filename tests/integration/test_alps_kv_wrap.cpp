#include <gtest/gtest.h>

#include "compat/alps_kv_channel.h"

#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <future>
#include <vector>

namespace {

using zerokv::compat::AlpsKvChannel;

bool MakeConnectedServerClient(std::unique_ptr<AlpsKvChannel>* server_out,
                               std::unique_ptr<AlpsKvChannel>* client_out) {
    auto server = std::make_unique<AlpsKvChannel>();
    if (!server->Listen("127.0.0.1:0", 5000) || server->local_address().empty()) {
        return false;
    }

    auto client = std::make_unique<AlpsKvChannel>();
    if (!client->Connect(server->local_address(), 5000)) {
        return false;
    }

    *server_out = std::move(server);
    *client_out = std::move(client);
    return true;
}

}  // namespace

TEST(AlpsKvWrapTest, WriteReadSingleMessage) {
    std::unique_ptr<AlpsKvChannel> server;
    std::unique_ptr<AlpsKvChannel> client;
    ASSERT_TRUE(MakeConnectedServerClient(&server, &client));
    ASSERT_NE(server, nullptr);
    ASSERT_NE(client, nullptr);

    const std::string payload = "hello-alps-wrap";
    std::vector<char> recv_buffer(payload.size(), '\0');

    std::thread reader([&]() {
        server->ReadBytes(recv_buffer.data(), recv_buffer.size(), 7, 3, 11, 12);
    });

    ASSERT_TRUE(client->WriteBytes(payload.data(), payload.size(), 7, 3, 11, 12));
    reader.join();

    EXPECT_EQ(std::memcmp(recv_buffer.data(), payload.data(), payload.size()), 0);
}

TEST(AlpsKvWrapTest, ReadBytesBatchConsumesMultipleMessages) {
    std::unique_ptr<AlpsKvChannel> server;
    std::unique_ptr<AlpsKvChannel> client;
    ASSERT_TRUE(MakeConnectedServerClient(&server, &client));
    ASSERT_NE(server, nullptr);
    ASSERT_NE(client, nullptr);

    const std::string payload_a = "message-a";
    const std::string payload_b = "message-bb";
    std::vector<char> recv_a(payload_a.size(), '\0');
    std::vector<char> recv_b(payload_b.size(), '\0');

    ASSERT_TRUE(client->WriteBytes(payload_a.data(), payload_a.size(), 1, 0, 2, 3));
    ASSERT_TRUE(client->WriteBytes(payload_b.data(), payload_b.size(), 2, 0, 2, 3));

    std::vector<void*> data{recv_a.data(), recv_b.data()};
    std::vector<size_t> sizes{recv_a.size(), recv_b.size()};
    std::vector<int> tags{1, 2};
    std::vector<int> indices{0, 0};
    std::vector<int> srcs{2, 2};
    std::vector<int> dsts{3, 3};

    server->ReadBytesBatch(data, sizes, tags, indices, srcs, dsts);

    EXPECT_EQ(std::memcmp(recv_a.data(), payload_a.data(), payload_a.size()), 0);
    EXPECT_EQ(std::memcmp(recv_b.data(), payload_b.data(), payload_b.size()), 0);
}

TEST(AlpsKvWrapTest, ShutdownCompletesAfterPeerDisconnect) {
    std::unique_ptr<AlpsKvChannel> server;
    std::unique_ptr<AlpsKvChannel> client;
    ASSERT_TRUE(MakeConnectedServerClient(&server, &client));
    ASSERT_NE(server, nullptr);
    ASSERT_NE(client, nullptr);

    const std::string payload = "disconnect-check";
    std::vector<char> recv_buffer(payload.size(), '\0');

    std::thread reader([&]() {
        server->ReadBytes(recv_buffer.data(), recv_buffer.size(), 9, 0, 1, 2);
    });

    ASSERT_TRUE(client->WriteBytes(payload.data(), payload.size(), 9, 0, 1, 2));
    reader.join();
    EXPECT_EQ(std::memcmp(recv_buffer.data(), payload.data(), payload.size()), 0);

    client->Shutdown();

    auto shutdown_future = std::async(std::launch::async, [&]() {
        server->Shutdown();
    });
    EXPECT_EQ(shutdown_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
}
