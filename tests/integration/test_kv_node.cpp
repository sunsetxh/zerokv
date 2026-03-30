#include "axon/kv.h"
#include "kv/protocol.h"
#include "kv/tcp_framing.h"
#include "kv/tcp_transport.h"

#include <gtest/gtest.h>

#include <cstring>
#include <cstdint>
#include <vector>

namespace {

using axon::kv::KVNode;
using axon::kv::KVServer;
using axon::kv::NodeConfig;
using axon::kv::ServerConfig;
namespace proto = axon::kv::detail;

std::optional<proto::GetMetaResponse> get_meta(const std::string& server_addr,
                                               const std::string& key,
                                               uint64_t request_id = 100) {
    std::string error;
    int fd = proto::TcpTransport::connect(server_addr, &error);
    if (fd < 0) {
        return std::nullopt;
    }

    proto::GetMetaRequest get;
    get.key = key;
    if (!proto::send_frame(fd, proto::MsgType::kGetMeta, request_id, proto::encode(get))) {
        proto::TcpTransport::close_fd(&fd);
        return std::nullopt;
    }

    proto::MsgHeader header;
    std::vector<uint8_t> payload;
    if (!proto::recv_frame(fd, &header, &payload)) {
        proto::TcpTransport::close_fd(&fd);
        return std::nullopt;
    }
    proto::TcpTransport::close_fd(&fd);

    if (static_cast<proto::MsgType>(header.type) != proto::MsgType::kGetMetaResp) {
        return std::nullopt;
    }
    return proto::decode_get_meta_response(payload);
}

TEST(KvNodeIntegrationTest, StartRegistersNodeAndStopDropsLiveness) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto node = KVNode::create(cfg);
    ASSERT_NE(node, nullptr);
    ASSERT_TRUE(node->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:20001",
        .node_id = "node-a",
    }).ok());
    EXPECT_TRUE(node->is_running());
    EXPECT_EQ(node->node_id(), "node-a");
    EXPECT_EQ(node->published_count(), 0u);

    std::string error;
    int fd = proto::TcpTransport::connect(server->address(), &error);
    ASSERT_GE(fd, 0) << error;

    proto::PutMetaRequest put;
    put.metadata.key = "alpha";
    put.metadata.owner_node_id = node->node_id();
    put.metadata.owner_data_addr = "127.0.0.1:20001";
    put.metadata.remote_addr = 0x1234;
    put.metadata.rkey = {1, 2, 3, 4};
    put.metadata.size = 128;
    put.metadata.version = 1;
    ASSERT_TRUE(proto::send_frame(fd, proto::MsgType::kPutMeta, 1, proto::encode(put)));

    proto::MsgHeader put_header;
    std::vector<uint8_t> put_payload;
    ASSERT_TRUE(proto::recv_frame(fd, &put_header, &put_payload));
    auto put_resp = proto::decode_put_meta_response(put_payload);
    ASSERT_TRUE(put_resp.has_value());
    EXPECT_EQ(put_resp->status, proto::MsgStatus::kOk);

    node->stop();
    EXPECT_FALSE(node->is_running());

    proto::GetMetaRequest get;
    get.key = "alpha";
    ASSERT_TRUE(proto::send_frame(fd, proto::MsgType::kGetMeta, 2, proto::encode(get)));

    proto::MsgHeader get_header;
    std::vector<uint8_t> get_payload;
    ASSERT_TRUE(proto::recv_frame(fd, &get_header, &get_payload));
    auto get_resp = proto::decode_get_meta_response(get_payload);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, proto::MsgStatus::kNotFound);

    proto::TcpTransport::close_fd(&fd);
    server->stop();
}

TEST(KvNodeIntegrationTest, MetricsAreEmptyBeforeAnyOperation) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto node = KVNode::create(cfg);
    ASSERT_NE(node, nullptr);
    ASSERT_TRUE(node->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:20007",
        .node_id = "metrics-empty-node",
    }).ok());

    auto publish_metrics = node->last_publish_metrics();
    auto fetch_metrics = node->last_fetch_metrics();
    EXPECT_FALSE(publish_metrics.has_value());
    EXPECT_FALSE(fetch_metrics.has_value());

    node->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, StartGeneratesNodeIdWhenMissing) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto node = KVNode::create(cfg);
    ASSERT_NE(node, nullptr);
    ASSERT_TRUE(node->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:20002",
        .node_id = {},
    }).ok());
    EXPECT_TRUE(node->is_running());
    EXPECT_FALSE(node->node_id().empty());

    std::string error;
    int fd = proto::TcpTransport::connect(server->address(), &error);
    ASSERT_GE(fd, 0) << error;

    proto::PutMetaRequest put;
    put.metadata.key = "beta";
    put.metadata.owner_node_id = node->node_id();
    put.metadata.owner_data_addr = "127.0.0.1:20002";
    put.metadata.remote_addr = 0x2345;
    put.metadata.rkey = {9, 8, 7};
    put.metadata.size = 64;
    put.metadata.version = 1;
    ASSERT_TRUE(proto::send_frame(fd, proto::MsgType::kPutMeta, 3, proto::encode(put)));

    proto::MsgHeader put_header;
    std::vector<uint8_t> put_payload;
    ASSERT_TRUE(proto::recv_frame(fd, &put_header, &put_payload));
    auto put_resp = proto::decode_put_meta_response(put_payload);
    ASSERT_TRUE(put_resp.has_value());
    EXPECT_EQ(put_resp->status, proto::MsgStatus::kOk);

    proto::TcpTransport::close_fd(&fd);
    node->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, PublishRegistersMetadataAndTracksCount) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto node = KVNode::create(cfg);
    ASSERT_NE(node, nullptr);
    ASSERT_TRUE(node->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:21001",
        .node_id = "publisher-a",
    }).ok());

    std::string value = "hello-rdma";
    auto publish = node->publish("alpha", value.data(), value.size());
    EXPECT_TRUE(publish.status().ok());
    publish.get();

    EXPECT_EQ(node->published_count(), 1u);

    auto info = server->lookup("alpha");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->key, "alpha");
    EXPECT_EQ(info->size, value.size());
    EXPECT_EQ(info->version, 1u);

    std::string updated = "hello-rdma-v2";
    auto republish = node->publish("alpha", updated.data(), updated.size());
    EXPECT_TRUE(republish.status().ok());
    republish.get();

    EXPECT_EQ(node->published_count(), 1u);

    info = server->lookup("alpha");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->size, updated.size());
    EXPECT_EQ(info->version, 2u);

    node->stop();
    EXPECT_EQ(node->published_count(), 0u);
    server->stop();
}

TEST(KvNodeIntegrationTest, PublishMetricsAreRecordedAndOverwritten) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto node = KVNode::create(cfg);
    ASSERT_NE(node, nullptr);
    ASSERT_TRUE(node->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:21012",
        .node_id = "publisher-metrics",
    }).ok());

    const std::string first_value = "metrics-publish-1";
    auto first = node->publish("metrics-key-1", first_value.data(), first_value.size());
    ASSERT_TRUE(first.status().ok());
    first.get();

    auto first_metrics = node->last_publish_metrics();
    ASSERT_TRUE(first_metrics.has_value());
    EXPECT_TRUE(first_metrics->ok);
    EXPECT_GT(first_metrics->total_us, 0u);
    EXPECT_GT(first_metrics->prepare_region_us, 0u);
    EXPECT_GT(first_metrics->pack_rkey_us, 0u);
    EXPECT_GT(first_metrics->put_meta_rpc_us, 0u);

    const std::string second_value = "metrics-publish-2";
    auto second = node->publish("metrics-key-2", second_value.data(), second_value.size());
    ASSERT_TRUE(second.status().ok());
    second.get();

    auto second_metrics = node->last_publish_metrics();
    ASSERT_TRUE(second_metrics.has_value());
    EXPECT_TRUE(second_metrics->ok);
    EXPECT_GT(second_metrics->total_us, 0u);
    EXPECT_GT(second_metrics->prepare_region_us, 0u);
    EXPECT_GT(second_metrics->pack_rkey_us, 0u);
    EXPECT_GT(second_metrics->put_meta_rpc_us, 0u);

    node->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, UnpublishRemovesLocalAndServerState) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto node = KVNode::create(cfg);
    ASSERT_NE(node, nullptr);
    ASSERT_TRUE(node->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:21011",
        .node_id = "publisher-unpublish",
    }).ok());

    const std::string value = "to-be-removed";
    auto publish = node->publish("ephemeral-key", value.data(), value.size());
    ASSERT_TRUE(publish.status().ok());
    publish.get();
    ASSERT_EQ(node->published_count(), 1u);
    ASSERT_TRUE(server->lookup("ephemeral-key").has_value());

    auto unpublish = node->unpublish("ephemeral-key");
    EXPECT_TRUE(unpublish.status().ok());
    unpublish.get();

    EXPECT_EQ(node->published_count(), 0u);
    EXPECT_FALSE(server->lookup("ephemeral-key").has_value());

    node->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, PublishRegionUsesCallerRegionAddress) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto node = KVNode::create(cfg);
    ASSERT_NE(node, nullptr);
    ASSERT_TRUE(node->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:21002",
        .node_id = "publisher-b",
    }).ok());

    auto ctx = axon::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = axon::MemoryRegion::allocate(ctx, 256);
    ASSERT_NE(region, nullptr);

    auto publish = node->publish_region("beta", region, 128);
    EXPECT_TRUE(publish.status().ok());
    publish.get();

    EXPECT_EQ(node->published_count(), 1u);

    auto meta = get_meta(server->address(), "beta", 200);
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->status, proto::MsgStatus::kOk);
    ASSERT_TRUE(meta->metadata.has_value());
    EXPECT_EQ(meta->metadata->size, 128u);
    EXPECT_EQ(meta->metadata->version, 1u);
    EXPECT_EQ(meta->metadata->remote_addr,
              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(region->address())));

    node->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, FetchReturnsPublishedBytesAcrossNodes) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto publisher = KVNode::create(cfg);
    auto reader = KVNode::create(cfg);
    ASSERT_NE(publisher, nullptr);
    ASSERT_NE(reader, nullptr);

    ASSERT_TRUE(publisher->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:22001",
        .node_id = "publisher-fetch",
    }).ok());
    ASSERT_TRUE(reader->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:22002",
        .node_id = "reader-fetch",
    }).ok());

    const std::string value = "value-from-publisher";
    auto publish = publisher->publish("shared-key", value.data(), value.size());
    ASSERT_TRUE(publish.status().ok());
    publish.get();

    auto fetched = reader->fetch("shared-key");
    ASSERT_TRUE(fetched.status().ok());
    auto result = fetched.get();

    EXPECT_EQ(result.owner_node_id, "publisher-fetch");
    EXPECT_EQ(result.version, 1u);
    ASSERT_EQ(result.data.size(), value.size());
    EXPECT_EQ(std::memcmp(result.data.data(), value.data(), value.size()), 0);

    auto metrics = reader->last_fetch_metrics();
    ASSERT_TRUE(metrics.has_value());
    EXPECT_TRUE(metrics->ok);
    EXPECT_GT(metrics->total_us, 0u);
    EXPECT_GT(metrics->local_buffer_prepare_us, 0u);
    EXPECT_GT(metrics->get_meta_rpc_us, 0u);
    EXPECT_GT(metrics->peer_connect_us, 0u);
    EXPECT_GT(metrics->rdma_prepare_us, 0u);
    EXPECT_GT(metrics->rdma_get_us, 0u);

    reader->stop();
    publisher->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, FetchToWritesIntoCallerRegion) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto publisher = KVNode::create(cfg);
    auto reader = KVNode::create(cfg);
    ASSERT_NE(publisher, nullptr);
    ASSERT_NE(reader, nullptr);

    ASSERT_TRUE(publisher->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:22003",
        .node_id = "publisher-fetch-to",
    }).ok());
    ASSERT_TRUE(reader->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:22004",
        .node_id = "reader-fetch-to",
    }).ok());

    const std::string value = "fetch-to-payload";
    auto publish = publisher->publish("fetch-to-key", value.data(), value.size());
    ASSERT_TRUE(publish.status().ok());
    publish.get();

    auto ctx = axon::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = axon::MemoryRegion::allocate(ctx, 128);
    ASSERT_NE(region, nullptr);

    auto fetch = reader->fetch_to("fetch-to-key", region, region->length(), 8);
    ASSERT_TRUE(fetch.status().ok());
    fetch.get();

    const auto* bytes = static_cast<const char*>(region->address());
    EXPECT_EQ(std::memcmp(bytes + 8, value.data(), value.size()), 0);

    auto metrics = reader->last_fetch_metrics();
    ASSERT_TRUE(metrics.has_value());
    EXPECT_TRUE(metrics->ok);
    EXPECT_GT(metrics->rdma_get_us, 0u);

    reader->stop();
    publisher->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, FetchFailsAfterUnpublish) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto publisher = KVNode::create(cfg);
    auto reader = KVNode::create(cfg);
    ASSERT_NE(publisher, nullptr);
    ASSERT_NE(reader, nullptr);

    ASSERT_TRUE(publisher->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:22005",
        .node_id = "publisher-unpublish-fetch",
    }).ok());
    ASSERT_TRUE(reader->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:22006",
        .node_id = "reader-unpublish-fetch",
    }).ok());

    const std::string value = "temporary-value";
    auto publish = publisher->publish("temp-key", value.data(), value.size());
    ASSERT_TRUE(publish.status().ok());
    publish.get();

    auto ok_fetch = reader->fetch("temp-key");
    ASSERT_TRUE(ok_fetch.status().ok());
    auto first = ok_fetch.get();
    ASSERT_EQ(first.data.size(), value.size());
    EXPECT_EQ(std::memcmp(first.data.data(), value.data(), value.size()), 0);

    auto unpublish = publisher->unpublish("temp-key");
    ASSERT_TRUE(unpublish.status().ok());
    unpublish.get();

    auto missing_fetch = reader->fetch("temp-key");
    EXPECT_FALSE(missing_fetch.status().ok());

    auto missing_metrics = reader->last_fetch_metrics();
    ASSERT_TRUE(missing_metrics.has_value());
    EXPECT_FALSE(missing_metrics->ok);
    EXPECT_GT(missing_metrics->get_meta_rpc_us, 0u);

    reader->stop();
    publisher->stop();
    server->stop();
}

}  // namespace
