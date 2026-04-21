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

TEST(AlpsKvWrapTest, WriteBytesUsesRmaPutInsteadOfPayloadTagSend) {
    std::unique_ptr<AlpsKvChannel> server;
    std::unique_ptr<AlpsKvChannel> client;
    ASSERT_TRUE(MakeConnectedServerClient(&server, &client));
    ASSERT_NE(server, nullptr);
    ASSERT_NE(client, nullptr);

    const std::string payload = "put-path-check";
    std::vector<char> recv_buffer(payload.size(), '\0');

    std::thread reader([&]() {
        server->ReadBytes(recv_buffer.data(), recv_buffer.size(), 5, 1, 8, 9);
    });

    ASSERT_TRUE(client->WriteBytes(payload.data(), payload.size(), 5, 1, 8, 9));
    reader.join();

    EXPECT_EQ(std::memcmp(recv_buffer.data(), payload.data(), payload.size()), 0);
    const auto stats = client->debug_stats();
    EXPECT_EQ(stats.payload_tag_send_ops, 0u);
    EXPECT_GT(stats.rma_put_ops, 0u);
}

TEST(AlpsKvWrapTest, WriteBytesCollectsTimingBreakdown) {
    std::unique_ptr<AlpsKvChannel> server;
    std::unique_ptr<AlpsKvChannel> client;
    ASSERT_TRUE(MakeConnectedServerClient(&server, &client));
    ASSERT_NE(server, nullptr);
    ASSERT_NE(client, nullptr);

    const std::string payload = "timing-breakdown";
    std::vector<char> recv_buffer(payload.size(), '\0');

    client->reset_write_timing_stats();
    std::thread reader([&]() {
        server->ReadBytes(recv_buffer.data(), recv_buffer.size(), 6, 1, 4, 5);
    });

    ASSERT_TRUE(client->WriteBytes(payload.data(), payload.size(), 6, 1, 4, 5));
    reader.join();

    const auto timing = client->write_timing_stats();
    EXPECT_EQ(timing.write_ops, 1u);
    EXPECT_GT(timing.control_request_grant_us, 0u);
    EXPECT_GT(timing.write_done_us, 0u);
}

TEST(AlpsKvWrapTest, ReadBytesCollectsReceivePathStats) {
    std::unique_ptr<AlpsKvChannel> server;
    std::unique_ptr<AlpsKvChannel> client;
    ASSERT_TRUE(MakeConnectedServerClient(&server, &client));
    ASSERT_NE(server, nullptr);
    ASSERT_NE(client, nullptr);

    const std::string payload = "receive-path";
    std::vector<char> recv_buffer(payload.size(), '\0');

    server->reset_receive_path_stats();
    std::thread reader([&]() {
        server->ReadBytes(recv_buffer.data(), recv_buffer.size(), 15, 0, 1, 2);
    });

    ASSERT_TRUE(client->WriteBytes(payload.data(), payload.size(), 15, 0, 1, 2));
    reader.join();

    const auto stats = server->receive_path_stats();
    EXPECT_EQ(stats.direct_grant_ops, 1u);
    EXPECT_EQ(stats.staged_grant_ops, 0u);
    EXPECT_EQ(stats.staged_delivery_ops, 0u);
    EXPECT_EQ(stats.staged_copy_bytes, 0u);
    EXPECT_EQ(stats.staged_copy_us, 0u);
}

TEST(AlpsKvWrapTest, ReusesReceiveRegistrationForSameBufferAcrossReads) {
    std::unique_ptr<AlpsKvChannel> server;
    std::unique_ptr<AlpsKvChannel> client;
    ASSERT_TRUE(MakeConnectedServerClient(&server, &client));
    ASSERT_NE(server, nullptr);
    ASSERT_NE(client, nullptr);

    const std::string payload_a = "reuse-recv-a";
    const std::string payload_b = "reuse-recv-b";
    ASSERT_EQ(payload_a.size(), payload_b.size());
    std::vector<char> recv_buffer(payload_a.size(), '\0');

    std::thread reader_a([&]() {
        server->ReadBytes(recv_buffer.data(), recv_buffer.size(), 10, 0, 1, 2);
    });
    ASSERT_TRUE(client->WriteBytes(payload_a.data(), payload_a.size(), 10, 0, 1, 2));
    reader_a.join();

    std::thread reader_b([&]() {
        server->ReadBytes(recv_buffer.data(), recv_buffer.size(), 11, 0, 1, 2);
    });
    ASSERT_TRUE(client->WriteBytes(payload_b.data(), payload_b.size(), 11, 0, 1, 2));
    reader_b.join();

    const auto stats = server->debug_stats();
    EXPECT_EQ(stats.receive_slot_register_ops, 1u);
}

TEST(AlpsKvWrapTest, ReusesRemoteKeyUnpackForSameReceiveBufferAcrossWrites) {
    std::unique_ptr<AlpsKvChannel> server;
    std::unique_ptr<AlpsKvChannel> client;
    ASSERT_TRUE(MakeConnectedServerClient(&server, &client));
    ASSERT_NE(server, nullptr);
    ASSERT_NE(client, nullptr);

    const std::string payload = "reuse-remote-rkey";
    std::vector<char> recv_buffer(payload.size(), '\0');

    std::thread reader_a([&]() {
        server->ReadBytes(recv_buffer.data(), recv_buffer.size(), 12, 0, 1, 2);
    });
    ASSERT_TRUE(client->WriteBytes(payload.data(), payload.size(), 12, 0, 1, 2));
    reader_a.join();

    std::thread reader_b([&]() {
        server->ReadBytes(recv_buffer.data(), recv_buffer.size(), 13, 0, 1, 2);
    });
    ASSERT_TRUE(client->WriteBytes(payload.data(), payload.size(), 13, 0, 1, 2));
    reader_b.join();

    const auto stats = client->debug_stats();
    EXPECT_EQ(stats.remote_rkey_unpack_ops, 1u);
}
