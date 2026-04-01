#include "zerokv/message_kv.h"

#include <cstring>
#include <algorithm>
#include <gtest/gtest.h>

class MessageKvIntegrationTest : public ::testing::Test {
protected:
    zerokv::Config cfg = zerokv::Config::builder().set_transport("tcp").build();
};

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
