#include "axon/kv.h"

#include <gtest/gtest.h>

TEST(KvBenchIntegrationTest, HoldOwnerPublishesStableKeys) {
    const auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = axon::kv::KVServer::create(cfg);
    ASSERT_TRUE(server->start(axon::kv::ServerConfig{"127.0.0.1:0"}).ok());

    auto owner = axon::kv::KVNode::create(cfg);
    ASSERT_TRUE(owner->start(axon::kv::NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "bench-owner",
    }).ok());

    auto publish = owner->publish("bench-fetch-4096", "xxxx", 4);
    ASSERT_TRUE(publish.status().ok());
    publish.get();

    auto info = server->lookup("bench-fetch-4096");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->key, "bench-fetch-4096");

    owner->stop();
    server->stop();
}
