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

TEST(KvBenchIntegrationTest, PublishBenchmarkCompletesSingleSizeSweep) {
    const auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = axon::kv::KVServer::create(cfg);
    ASSERT_TRUE(server->start(axon::kv::ServerConfig{"127.0.0.1:0"}).ok());

    auto node = axon::kv::KVNode::create(cfg);
    ASSERT_TRUE(node->start(axon::kv::NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "bench-publish-node",
    }).ok());

    const std::string key = "bench-publish-4096-0";
    std::vector<std::byte> payload(4096, std::byte{0x5a});
    auto publish = node->publish(key, payload.data(), payload.size());
    ASSERT_TRUE(publish.status().ok());
    publish.get();

    auto metrics = node->last_publish_metrics();
    ASSERT_TRUE(metrics.has_value());
    EXPECT_GT(metrics->total_us, 0u);

    auto unpublish = node->unpublish(key);
    ASSERT_TRUE(unpublish.status().ok());
    unpublish.get();

    node->stop();
    server->stop();
}
