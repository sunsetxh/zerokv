#include "zerokv/core/kv_node.h"
#include "zerokv/core/kv_server.h"
#include "zerokv/kv.h"

#include <algorithm>
#include <future>
#include <cstring>
#include <functional>
#include <gtest/gtest.h>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

class KvIntegrationTest : public ::testing::Test {
protected:
    zerokv::Config cfg = zerokv::Config::builder().set_transport("tcp").build();
};

namespace {

void expect_system_error_code(const std::function<void()>& fn, zerokv::ErrorCode code) {
    try {
        fn();
        FAIL() << "expected std::system_error";
    } catch (const std::system_error& e) {
        EXPECT_EQ(e.code(), make_error_code(code));
    }
}

}  // namespace

TEST(KvApiSurfaceTest, PublicTypesExist) {
    using zerokv::KV;

    KV::BatchRecvItem item;
    item.key = "k";
    item.length = 16;
    item.offset = 0;

    KV::BatchRecvResult result;
    EXPECT_TRUE(result.completed.empty());
    EXPECT_FALSE(result.completed_all);
}

TEST(KvApiSurfaceTest, AsyncSenderMethodsExist) {
    using FutureType = zerokv::transport::Future<void>;
    static_assert(std::is_same_v<decltype(std::declval<zerokv::KV&>().send_async(
                      std::declval<const std::string&>(),
                      std::declval<const void*>(),
                      std::declval<size_t>())),
                  FutureType>);
    static_assert(std::is_same_v<decltype(std::declval<zerokv::KV&>().send_region_async(
                      std::declval<const std::string&>(),
                      std::declval<const zerokv::transport::MemoryRegion::Ptr&>(),
                      std::declval<size_t>())),
                  FutureType>);
}

TEST(KvCoreApiSurfaceTest, CoreHeadersCompile) {
    using zerokv::core::KVNode;
    using zerokv::core::KVServer;
    using zerokv::core::NodeConfig;
    using zerokv::core::ServerConfig;

    NodeConfig node_cfg;
    ServerConfig server_cfg;
    (void)node_cfg;
    (void)server_cfg;
}

TEST_F(KvIntegrationTest, AllocateSendRegionRequiresRunningNode) {
    auto mq = zerokv::KV::create(cfg);
    expect_system_error_code([&] { (void)mq->allocate_send_region(1024); },
                             zerokv::ErrorCode::kConnectionRefused);
}

TEST_F(KvIntegrationTest, AllocateSendRegionSucceedsAfterStart) {
    auto mq = zerokv::KV::create(cfg);
    auto server = zerokv::core::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    mq->start({server->address(), "127.0.0.1:0", "sender"});
    auto region = mq->allocate_send_region(1024);
    ASSERT_NE(region, nullptr);
    EXPECT_GE(region->length(), 1024u);

    mq->stop();
    server->stop();
}

TEST_F(KvIntegrationTest, SendRejectsEmptyKey) {
    auto mq = zerokv::KV::create(cfg);

    auto ctx = zerokv::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = zerokv::transport::MemoryRegion::allocate(ctx, 8);
    ASSERT_NE(region, nullptr);

    expect_system_error_code([&] { mq->send("", "x", 1); },
                             zerokv::ErrorCode::kInvalidArgument);
    expect_system_error_code([&] { mq->send_region("", region, 1); },
                             zerokv::ErrorCode::kInvalidArgument);
}

TEST_F(KvIntegrationTest, RecvRejectsEmptyKey) {
    auto mq = zerokv::KV::create(cfg);

    auto ctx = zerokv::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = zerokv::transport::MemoryRegion::allocate(ctx, 8);
    ASSERT_NE(region, nullptr);

    expect_system_error_code(
        [&] { mq->recv("", region, 1, 0, std::chrono::milliseconds(1)); },
        zerokv::ErrorCode::kInvalidArgument);
}

TEST_F(KvIntegrationTest, RecvBatchRejectsInvalidLayoutBeforeWaiting) {
    auto mq = zerokv::KV::create(cfg);
    auto server = zerokv::core::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());
    mq->start({server->address(), "127.0.0.1:0", "receiver"});

    auto ctx = zerokv::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = zerokv::transport::MemoryRegion::allocate(ctx, 16);
    ASSERT_NE(region, nullptr);

    expect_system_error_code(
        [&] {
            (void)mq->recv_batch({
                {"a", 8, 0},
                {"b", 8, 4},
            }, region, std::chrono::milliseconds(1));
        },
        zerokv::ErrorCode::kInvalidArgument);

    mq->stop();
    server->stop();
}

TEST_F(KvIntegrationTest, StopBeforeStartIsSafe) {
    auto mq = zerokv::KV::create(cfg);
    EXPECT_NO_THROW(mq->stop());
}

TEST_F(KvIntegrationTest, StartAndStopAreIdempotentEnoughForSingleLifecycle) {
    auto server = zerokv::core::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto mq = zerokv::KV::create(cfg);
    EXPECT_NO_THROW(mq->start({server->address(), "127.0.0.1:0", "mq-node"}));
    EXPECT_NO_THROW(mq->stop());

    server->stop();
}

TEST_F(KvIntegrationTest, SenderCleanupRunsOnSubsequentSend) {
    auto server = zerokv::core::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto sender = zerokv::KV::create(cfg);
    auto receiver = zerokv::KV::create(cfg);
    sender->start({server->address(), "127.0.0.1:0", "sender"});
    receiver->start({server->address(), "127.0.0.1:0", "receiver"});

    auto ctx = zerokv::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto rx_region = zerokv::transport::MemoryRegion::allocate(ctx, 16);
    ASSERT_NE(rx_region, nullptr);

    std::thread recv_thread([&] {
        receiver->recv("msg-1", rx_region, 5, 0, std::chrono::milliseconds(1000));
    });
    sender->send("msg-1", "hello", 5);
    recv_thread.join();
    EXPECT_EQ(std::memcmp(rx_region->address(), "hello", 5), 0);

    std::thread recv_thread2([&] {
        receiver->recv("msg-2", rx_region, 5, 0, std::chrono::milliseconds(1000));
    });
    sender->send("msg-2", "world", 5);
    recv_thread2.join();

    auto keys = server->list_keys();
    EXPECT_EQ(std::find(keys.begin(), keys.end(), "msg-1"), keys.end());
    EXPECT_EQ(std::find(keys.begin(), keys.end(), "msg-2"), keys.end());

    sender->stop();
    receiver->stop();
    server->stop();
}

TEST_F(KvIntegrationTest, RecvCopiesSingleMessageIntoRegion) {
    auto server = zerokv::core::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto sender = zerokv::KV::create(cfg);
    auto receiver = zerokv::KV::create(cfg);
    sender->start({server->address(), "127.0.0.1:0", "sender"});
    receiver->start({server->address(), "127.0.0.1:0", "receiver"});

    auto ctx = zerokv::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = zerokv::transport::MemoryRegion::allocate(ctx, 8);
    ASSERT_NE(region, nullptr);

    std::thread recv_thread([&] {
        receiver->recv("rx-key", region, 7, 0, std::chrono::milliseconds(1000));
    });
    sender->send("rx-key", "payload", 7);
    recv_thread.join();

    EXPECT_EQ(std::memcmp(region->address(), "payload", 7), 0);

    sender->stop();
    receiver->stop();
    server->stop();
}

TEST_F(KvIntegrationTest, RecvBatchReturnsPartialTimeout) {
    auto server = zerokv::core::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto sender = zerokv::KV::create(cfg);
    auto receiver = zerokv::KV::create(cfg);
    sender->start({server->address(), "127.0.0.1:0", "sender"});
    receiver->start({server->address(), "127.0.0.1:0", "receiver"});

    auto ctx = zerokv::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = zerokv::transport::MemoryRegion::allocate(ctx, 16);
    ASSERT_NE(region, nullptr);

    std::thread sender_thread([&] {
        sender->send("batch-a", "aaaa", 4);
    });
    auto result = receiver->recv_batch({
        {"batch-a", 4, 0},
        {"batch-b", 4, 8},
    }, region, std::chrono::milliseconds(100));
    sender_thread.join();

    EXPECT_EQ(result.completed.size(), 1u);
    EXPECT_EQ(result.completed[0], "batch-a");
    EXPECT_EQ(result.timed_out.size(), 1u);
    EXPECT_EQ(result.timed_out[0], "batch-b");
    EXPECT_FALSE(result.completed_all);

    sender->stop();
    receiver->stop();
    server->stop();
}

TEST_F(KvIntegrationTest, RecvBatchAcknowledgesCompletedKeyBeforeBatchFinishes) {
    auto timeout_cfg = zerokv::Config::builder()
                           .set_transport("tcp")
                           .set_connect_timeout(std::chrono::milliseconds(1000))
                           .build();
    auto server = zerokv::core::KVServer::create(timeout_cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto sender_a = zerokv::KV::create(timeout_cfg);
    auto sender_b = zerokv::KV::create(timeout_cfg);
    auto receiver = zerokv::KV::create(timeout_cfg);
    sender_a->start({server->address(), "127.0.0.1:0", "sender-a"});
    sender_b->start({server->address(), "127.0.0.1:0", "sender-b"});
    receiver->start({server->address(), "127.0.0.1:0", "receiver"});

    auto ctx = zerokv::Context::create(timeout_cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = zerokv::transport::MemoryRegion::allocate(ctx, 16);
    ASSERT_NE(region, nullptr);

    std::promise<void> recv_started;
    std::thread recv_thread([&] {
        recv_started.set_value();
        auto result = receiver->recv_batch({
            {"batch-fast", 4, 0},
            {"batch-slow", 4, 8},
        }, region, std::chrono::milliseconds(2000));
        EXPECT_EQ(result.completed.size(), 2u);
        EXPECT_TRUE(result.failed.empty());
        EXPECT_TRUE(result.timed_out.empty());
        EXPECT_TRUE(result.completed_all);
    });
    recv_started.get_future().wait();

    auto send_a = std::async(std::launch::async, [&] {
        sender_a->send("batch-fast", "fast", 4);
    });

    EXPECT_EQ(send_a.wait_for(std::chrono::milliseconds(300)), std::future_status::ready);

    sender_b->send("batch-slow", "slow", 4);
    send_a.get();
    recv_thread.join();

    sender_a->stop();
    sender_b->stop();
    receiver->stop();
    server->stop();
}

TEST_F(KvIntegrationTest, SendUnpublishesMessageKeyAfterAck) {
    auto server = zerokv::core::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto sender = zerokv::KV::create(cfg);
    auto receiver = zerokv::KV::create(cfg);
    sender->start({server->address(), "127.0.0.1:0", "sender"});
    receiver->start({server->address(), "127.0.0.1:0", "receiver"});

    auto ctx = zerokv::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = zerokv::transport::MemoryRegion::allocate(ctx, 8);
    ASSERT_NE(region, nullptr);

    std::thread recv_thread([&] {
        receiver->recv("send-key", region, 5, 0, std::chrono::milliseconds(1000));
    });
    sender->send("send-key", "hello", 5);
    recv_thread.join();

    auto keys = server->list_keys();
    EXPECT_EQ(std::find(keys.begin(), keys.end(), "send-key"), keys.end());

    sender->stop();
    receiver->stop();
    server->stop();
}

TEST_F(KvIntegrationTest, SendRegionUnpublishesMessageKeyAfterAck) {
    auto server = zerokv::core::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto sender = zerokv::KV::create(cfg);
    auto receiver = zerokv::KV::create(cfg);
    sender->start({server->address(), "127.0.0.1:0", "sender"});
    receiver->start({server->address(), "127.0.0.1:0", "receiver"});

    auto ctx = zerokv::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = zerokv::transport::MemoryRegion::allocate(ctx, 16);
    ASSERT_NE(region, nullptr);
    std::memcpy(region->address(), "region-payload", 15);

    auto rx_region = zerokv::transport::MemoryRegion::allocate(ctx, 16);
    ASSERT_NE(rx_region, nullptr);

    std::thread recv_thread([&] {
        receiver->recv("send-region-key", rx_region, 15, 0, std::chrono::milliseconds(1000));
    });
    sender->send_region("send-region-key", region, 15);
    recv_thread.join();

    auto keys = server->list_keys();
    EXPECT_EQ(std::find(keys.begin(), keys.end(), "send-region-key"), keys.end());

    sender->stop();
    receiver->stop();
    server->stop();
}

TEST_F(KvIntegrationTest, SendRegionStillWaitsForAckBeforeReturning) {
    auto server = zerokv::core::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto sender = zerokv::KV::create(cfg);
    auto receiver = zerokv::KV::create(cfg);
    sender->start({server->address(), "127.0.0.1:0", "sender"});
    receiver->start({server->address(), "127.0.0.1:0", "receiver"});

    auto send_region = sender->allocate_send_region(8);
    ASSERT_NE(send_region, nullptr);
    std::memcpy(send_region->address(), "payload", 7);

    auto recv_region = zerokv::transport::MemoryRegion::allocate(
        zerokv::Context::create(cfg), 8);
    ASSERT_NE(recv_region, nullptr);

    std::atomic<bool> send_returned{false};
    std::thread send_thread([&] {
        sender->send_region("sync-key", send_region, 7);
        send_returned.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(send_returned.load());

    receiver->recv("sync-key", recv_region, 7, 0, std::chrono::seconds(5));
    send_thread.join();
    EXPECT_TRUE(send_returned.load());

    sender->stop();
    receiver->stop();
    server->stop();
}

TEST_F(KvIntegrationTest, SendRequiresRunningNodeAndValidatesInputs) {
    auto mq = zerokv::KV::create(cfg);
    expect_system_error_code([&] { mq->send("key", "x", 1); },
                             zerokv::ErrorCode::kConnectionRefused);

    auto server = zerokv::core::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());
    mq->start({server->address(), "127.0.0.1:0", "sender"});

    expect_system_error_code([&] { mq->send("", "x", 1); }, zerokv::ErrorCode::kInvalidArgument);
    expect_system_error_code([&] { mq->send("key", nullptr, 1); }, zerokv::ErrorCode::kInvalidArgument);

    mq->stop();
    server->stop();
}

TEST_F(KvIntegrationTest, SendTimesOutWithoutAckAndCleansUpMessageKey) {
    const auto timeout_cfg = zerokv::Config::builder()
                                 .set_transport("tcp")
                                 .set_connect_timeout(std::chrono::milliseconds(50))
                                 .build();
    auto server = zerokv::core::KVServer::create(timeout_cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto sender = zerokv::KV::create(timeout_cfg);
    sender->start({server->address(), "127.0.0.1:0", "sender"});

    expect_system_error_code([&] { sender->send("timeout-key", "x", 1); },
                             zerokv::ErrorCode::kTimeout);

    auto keys = server->list_keys();
    EXPECT_EQ(std::find(keys.begin(), keys.end(), "timeout-key"), keys.end());

    sender->stop();
    server->stop();
}

TEST_F(KvIntegrationTest, SendRegionRequiresRunningNodeAndValidatesInputs) {
    auto mq = zerokv::KV::create(cfg);
    auto ctx = zerokv::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = zerokv::transport::MemoryRegion::allocate(ctx, 8);
    ASSERT_NE(region, nullptr);

    expect_system_error_code([&] { mq->send_region("key", region, 1); },
                             zerokv::ErrorCode::kConnectionRefused);

    auto server = zerokv::core::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());
    mq->start({server->address(), "127.0.0.1:0", "sender"});

    expect_system_error_code([&] { mq->send_region("", region, 1); },
                             zerokv::ErrorCode::kInvalidArgument);
    expect_system_error_code([&] { mq->send_region("key", nullptr, 1); },
                             zerokv::ErrorCode::kInvalidArgument);
    expect_system_error_code([&] { mq->send_region("key", region, region->length() + 1); },
                             zerokv::ErrorCode::kInvalidArgument);

    mq->stop();
    server->stop();
}

TEST_F(KvIntegrationTest, SendRegionAsyncCompletesAfterAckAndCleanup) {
    auto server = zerokv::core::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto sender = zerokv::KV::create(cfg);
    auto receiver = zerokv::KV::create(cfg);
    sender->start({server->address(), "127.0.0.1:0", "sender"});
    receiver->start({server->address(), "127.0.0.1:0", "receiver"});

    auto region = sender->allocate_send_region(8);
    ASSERT_NE(region, nullptr);
    std::memcpy(region->address(), "payload", 7);

    auto recv_region = zerokv::transport::MemoryRegion::allocate(
        zerokv::Context::create(cfg), 8);
    ASSERT_NE(recv_region, nullptr);

    auto future = sender->send_region_async("async-key", region, 7);
    EXPECT_FALSE(future.ready());

    receiver->recv("async-key", recv_region, 7, 0, std::chrono::seconds(5));

    future.get();
    EXPECT_TRUE(future.status().ok());

    sender->stop();
    receiver->stop();
    server->stop();
}

TEST_F(KvIntegrationTest, SendRegionAsyncFailsIfStoppedBeforeAck) {
    auto server = zerokv::core::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto sender = zerokv::KV::create(cfg);
    sender->start({server->address(), "127.0.0.1:0", "sender"});

    auto region = sender->allocate_send_region(8);
    ASSERT_NE(region, nullptr);
    std::memcpy(region->address(), "payload", 7);

    auto future = sender->send_region_async("async-stop", region, 7);
    sender->stop();

    future.get();
    EXPECT_EQ(future.status().code(), zerokv::ErrorCode::kConnectionReset);

    server->stop();
}

TEST_F(KvIntegrationTest, AsyncSendCleanupCompletesByAckArrivalOrder) {
    auto server = zerokv::core::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto sender = zerokv::KV::create(cfg);
    auto receiver = zerokv::KV::create(cfg);
    sender->start({server->address(), "127.0.0.1:0", "sender"});
    receiver->start({server->address(), "127.0.0.1:0", "receiver"});

    auto region_a = sender->allocate_send_region(8);
    auto region_b = sender->allocate_send_region(8);
    ASSERT_NE(region_a, nullptr);
    ASSERT_NE(region_b, nullptr);
    std::memcpy(region_a->address(), "aaaaaaa", 7);
    std::memcpy(region_b->address(), "bbbbbbb", 7);

    auto recv_region = zerokv::transport::MemoryRegion::allocate(
        zerokv::Context::create(cfg), 16);
    ASSERT_NE(recv_region, nullptr);

    auto future_a = sender->send_region_async("async-a", region_a, 7);
    auto future_b = sender->send_region_async("async-b", region_b, 7);

    receiver->recv("async-b", recv_region, 7, 0, std::chrono::seconds(5));

    EXPECT_TRUE(future_b.get(std::chrono::milliseconds(500)).has_value());
    EXPECT_TRUE(future_b.status().ok());

    EXPECT_FALSE(future_a.get(std::chrono::milliseconds(50)).has_value());
    EXPECT_EQ(future_a.status().code(), zerokv::ErrorCode::kInProgress);

    receiver->recv("async-a", recv_region, 7, 8, std::chrono::seconds(5));
    future_a.get();
    EXPECT_TRUE(future_a.status().ok());

    sender->stop();
    receiver->stop();
    server->stop();
}
