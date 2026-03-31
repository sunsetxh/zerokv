#include "axon/kv.h"
#include "kv/bench_utils.h"

#include <gtest/gtest.h>

#include <cstring>

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

TEST(KvBenchIntegrationTest, PublishRegionBenchmarkPathCompletesSingleSizeSweep) {
    const auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = axon::kv::KVServer::create(cfg);
    ASSERT_TRUE(server->start(axon::kv::ServerConfig{"127.0.0.1:0"}).ok());

    auto node = axon::kv::KVNode::create(cfg);
    ASSERT_TRUE(node->start(axon::kv::NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "bench-publish-region-node",
    }).ok());

    auto ctx = axon::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = axon::MemoryRegion::allocate(ctx, 4096);
    ASSERT_NE(region, nullptr);
    std::memset(region->address(), 0x5a, region->length());

    const std::string key = "bench-publish-region-4096-0";
    auto publish = node->publish_region(key, region, region->length());
    ASSERT_TRUE(publish.status().ok());
    publish.get();

    auto metrics = node->last_publish_metrics();
    ASSERT_TRUE(metrics.has_value());
    EXPECT_GT(metrics->total_us, 0u);
    EXPECT_TRUE(metrics->ok);

    auto unpublish = node->unpublish(key);
    ASSERT_TRUE(unpublish.status().ok());
    unpublish.get();

    node->stop();
    server->stop();
}

TEST(KvBenchIntegrationTest, RenderedTablesUseMiBpsHeader) {
    std::vector<axon::kv::detail::PublishBenchRow> publish_rows{
        {.size_bytes = 4096, .iterations = 1, .avg_total_us = 1.0, .throughput_MiBps = 1.0},
    };
    std::vector<axon::kv::detail::FetchBenchRow> fetch_rows{
        {.size_bytes = 4096, .iterations = 1, .avg_total_us = 1.0, .throughput_MiBps = 1.0},
    };

    const auto publish_table = axon::kv::detail::render_publish_rows(publish_rows);
    const auto fetch_table = axon::kv::detail::render_fetch_rows(fetch_rows);

    EXPECT_NE(publish_table.find("throughput_MiBps"), std::string::npos);
    EXPECT_NE(fetch_table.find("throughput_MiBps"), std::string::npos);
    EXPECT_NE(fetch_table.find("avg_rkey_prepare_us"), std::string::npos);
    EXPECT_NE(fetch_table.find("avg_get_submit_us"), std::string::npos);
}

TEST(KvBenchIntegrationTest, FetchToSmoke) {
    const auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = axon::kv::KVServer::create(cfg);
    ASSERT_TRUE(server->start(axon::kv::ServerConfig{"127.0.0.1:0"}).ok());

    auto owner = axon::kv::KVNode::create(cfg);
    auto reader = axon::kv::KVNode::create(cfg);
    ASSERT_TRUE(owner->start(axon::kv::NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "bench-owner-fetch-to",
    }).ok());
    ASSERT_TRUE(reader->start(axon::kv::NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "bench-reader-fetch-to",
    }).ok());

    std::vector<std::byte> payload(4096, std::byte{0x2a});
    auto publish = owner->publish("bench-fetch-4096", payload.data(), payload.size());
    ASSERT_TRUE(publish.status().ok());
    publish.get();

    auto ctx = axon::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = axon::MemoryRegion::allocate(ctx, payload.size());
    ASSERT_NE(region, nullptr);

    auto fetch = reader->fetch_to("bench-fetch-4096", region, payload.size(), 0);
    ASSERT_TRUE(fetch.status().ok());
    fetch.get();

    auto metrics = reader->last_fetch_metrics();
    ASSERT_TRUE(metrics.has_value());
    EXPECT_TRUE(metrics->ok);
    EXPECT_GT(metrics->total_us, 0u);

    reader->stop();
    owner->stop();
    server->stop();
}
