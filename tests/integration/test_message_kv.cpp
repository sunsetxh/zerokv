#include "zerokv/message_kv.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <gtest/gtest.h>
#include <system_error>

class MessageKvIntegrationTest : public ::testing::Test {
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

TEST(MessageKvApiSurfaceTest, PublicTypesExist) {
    using zerokv::MessageKV;

    MessageKV::BatchRecvItem item;
    item.key = "k";
    item.length = 16;
    item.offset = 0;

    MessageKV::BatchRecvResult result;
    EXPECT_TRUE(result.completed.empty());
    EXPECT_FALSE(result.completed_all);
}

TEST_F(MessageKvIntegrationTest, StopBeforeStartIsSafe) {
    auto mq = zerokv::MessageKV::create(cfg);
    EXPECT_NO_THROW(mq->stop());
}

TEST_F(MessageKvIntegrationTest, StartAndStopAreIdempotentEnoughForSingleLifecycle) {
    auto server = zerokv::kv::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto mq = zerokv::MessageKV::create(cfg);
    EXPECT_NO_THROW(mq->start({server->address(), "127.0.0.1:0", "mq-node"}));
    EXPECT_NO_THROW(mq->stop());

    server->stop();
}

TEST_F(MessageKvIntegrationTest, SenderCleanupRunsOnSubsequentSend) {
    auto server = zerokv::kv::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto sender = zerokv::MessageKV::create(cfg);
    auto receiver = zerokv::MessageKV::create(cfg);
    sender->start({server->address(), "127.0.0.1:0", "sender"});
    receiver->start({server->address(), "127.0.0.1:0", "receiver"});

    auto ctx = zerokv::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto rx_region = zerokv::MemoryRegion::allocate(ctx, 16);
    ASSERT_NE(rx_region, nullptr);

    sender->send("msg-1", "hello", 5);
    receiver->recv("msg-1", rx_region, 5, 0, std::chrono::milliseconds(1000));
    EXPECT_EQ(std::memcmp(rx_region->address(), "hello", 5), 0);
    sender->send("msg-2", "world", 5);

    auto keys = server->list_keys();
    EXPECT_EQ(std::find(keys.begin(), keys.end(), "msg-1"), keys.end());
    EXPECT_NE(std::find(keys.begin(), keys.end(), "msg-2"), keys.end());

    sender->stop();
    receiver->stop();
    server->stop();
}

TEST_F(MessageKvIntegrationTest, SendPublishesMessageKey) {
    auto server = zerokv::kv::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto sender = zerokv::MessageKV::create(cfg);
    sender->start({server->address(), "127.0.0.1:0", "sender"});

    const char payload[] = "hello";
    sender->send("send-key", payload, 5);

    auto keys = server->list_keys();
    EXPECT_NE(std::find(keys.begin(), keys.end(), "send-key"), keys.end());

    sender->stop();
    server->stop();
}

TEST_F(MessageKvIntegrationTest, SendRegionPublishesMessageKey) {
    auto server = zerokv::kv::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto sender = zerokv::MessageKV::create(cfg);
    sender->start({server->address(), "127.0.0.1:0", "sender"});

    auto ctx = zerokv::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = zerokv::MemoryRegion::allocate(ctx, 16);
    ASSERT_NE(region, nullptr);
    std::memcpy(region->address(), "region-payload", 15);

    sender->send_region("send-region-key", region, 15);

    auto keys = server->list_keys();
    EXPECT_NE(std::find(keys.begin(), keys.end(), "send-region-key"), keys.end());

    sender->stop();
    server->stop();
}

TEST_F(MessageKvIntegrationTest, SendRequiresRunningNodeAndValidatesInputs) {
    auto mq = zerokv::MessageKV::create(cfg);
    expect_system_error_code([&] { mq->send("key", "x", 1); },
                             zerokv::ErrorCode::kConnectionRefused);

    auto server = zerokv::kv::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());
    mq->start({server->address(), "127.0.0.1:0", "sender"});

    expect_system_error_code([&] { mq->send("", "x", 1); }, zerokv::ErrorCode::kInvalidArgument);
    expect_system_error_code([&] { mq->send("key", nullptr, 1); }, zerokv::ErrorCode::kInvalidArgument);

    mq->stop();
    server->stop();
}

TEST_F(MessageKvIntegrationTest, SendRegionRequiresRunningNodeAndValidatesInputs) {
    auto mq = zerokv::MessageKV::create(cfg);
    auto ctx = zerokv::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = zerokv::MemoryRegion::allocate(ctx, 8);
    ASSERT_NE(region, nullptr);

    expect_system_error_code([&] { mq->send_region("key", region, 1); },
                             zerokv::ErrorCode::kConnectionRefused);

    auto server = zerokv::kv::KVServer::create(cfg);
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
