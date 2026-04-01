#include "zerokv/message_kv.h"

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
