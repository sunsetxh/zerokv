#include "axon/kv.h"
#include "kv/protocol.h"
#include "kv/tcp_framing.h"
#include "kv/tcp_transport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <cstdint>
#include <thread>
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

std::optional<proto::GetPushTargetResponse> get_push_target(const std::string& server_addr,
                                                            const std::string& target_node_id,
                                                            uint64_t request_id = 200) {
    std::string error;
    int fd = proto::TcpTransport::connect(server_addr, &error);
    if (fd < 0) {
        return std::nullopt;
    }

    proto::GetPushTargetRequest get;
    get.target_node_id = target_node_id;
    if (!proto::send_frame(fd, proto::MsgType::kGetPushTarget, request_id, proto::encode(get))) {
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

    if (static_cast<proto::MsgType>(header.type) != proto::MsgType::kGetPushTargetResp) {
        return std::nullopt;
    }
    return proto::decode_get_push_target_response(payload);
}

std::vector<axon::kv::SubscriptionEvent> drain_until_count(const KVNode::Ptr& node,
                                                           size_t expected_count,
                                                           std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<axon::kv::SubscriptionEvent> all;
    while (std::chrono::steady_clock::now() < deadline) {
        auto drained = node->drain_subscription_events();
        all.insert(all.end(), drained.begin(), drained.end());
        if (all.size() >= expected_count) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return all;
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

TEST(KvNodeIntegrationTest, DestructorStopsRunningNode) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    {
        auto node = KVNode::create(cfg);
        ASSERT_NE(node, nullptr);
        ASSERT_TRUE(node->start(NodeConfig{
            .server_addr = server->address(),
            .local_data_addr = "127.0.0.1:0",
            .node_id = "destructor-stop-node",
        }).ok());
        EXPECT_TRUE(node->is_running());
    }

    server->stop();
}

TEST(KvNodeIntegrationTest, StartFailsWithinConnectTimeoutWhenServerIsUnavailable) {
    auto cfg = axon::Config::builder()
                   .set_transport("tcp")
                   .set_connect_timeout(std::chrono::milliseconds(100))
                   .build();

    auto node = KVNode::create(cfg);
    ASSERT_NE(node, nullptr);

    const auto start_time = std::chrono::steady_clock::now();
    auto status = node->start(NodeConfig{
        .server_addr = "10.255.255.1:6553",
        .local_data_addr = "127.0.0.1:0",
        .node_id = "missing-server-timeout",
    });
    const auto elapsed = std::chrono::steady_clock::now() - start_time;

    EXPECT_FALSE(status.ok());
    EXPECT_TRUE(status.code() == axon::ErrorCode::kTimeout ||
                status.code() == axon::ErrorCode::kConnectionRefused);
    EXPECT_LT(elapsed, std::chrono::seconds(2));
    EXPECT_FALSE(node->is_running());

    node->stop();
}

TEST(KvNodeIntegrationTest, WaitForKeyReturnsWhenKeyAppears) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto waiter = KVNode::create(cfg);
    auto publisher = KVNode::create(cfg);
    ASSERT_TRUE(waiter->start(NodeConfig{server->address(), "127.0.0.1:0", "waiter-a"}).ok());
    ASSERT_TRUE(publisher->start(NodeConfig{server->address(), "127.0.0.1:0", "publisher-a"}).ok());

    std::thread publish_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::string value = "ready-value";
        auto publish = publisher->publish("wait-key", value.data(), value.size());
        ASSERT_TRUE(publish.status().ok());
        publish.get();
    });

    auto status = waiter->wait_for_key("wait-key", std::chrono::milliseconds(1000));
    EXPECT_TRUE(status.ok()) << status.message();

    publish_thread.join();
    publisher->stop();
    waiter->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, SubscribeAndFetchOnceFetchesPublishedKey) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto waiter = KVNode::create(cfg);
    auto publisher = KVNode::create(cfg);
    ASSERT_TRUE(waiter->start(NodeConfig{server->address(), "127.0.0.1:0", "waiter-b"}).ok());
    ASSERT_TRUE(publisher->start(NodeConfig{server->address(), "127.0.0.1:0", "publisher-b"}).ok());

    std::thread publish_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::string value = "fetch-once-value";
        auto publish = publisher->publish("fetch-once-key", value.data(), value.size());
        ASSERT_TRUE(publish.status().ok());
        publish.get();
    });

    auto result = waiter->subscribe_and_fetch_once("fetch-once-key", std::chrono::milliseconds(1000));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(result.data.data()), result.data.size()),
              "fetch-once-value");
    EXPECT_EQ(result.owner_node_id, "publisher-b");
    EXPECT_EQ(result.version, 1u);

    publish_thread.join();
    publisher->stop();
    waiter->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, WaitForKeysReturnsWhenAllKeysAppear) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto waiter = KVNode::create(cfg);
    auto publisher = KVNode::create(cfg);
    ASSERT_TRUE(waiter->start(NodeConfig{server->address(), "127.0.0.1:0", "waiter-c"}).ok());
    ASSERT_TRUE(publisher->start(NodeConfig{server->address(), "127.0.0.1:0", "publisher-c"}).ok());

    std::thread publish_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::string first = "value-a";
        auto first_publish = publisher->publish("batch-a", first.data(), first.size());
        ASSERT_TRUE(first_publish.status().ok());
        first_publish.get();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::string second = "value-b";
        auto second_publish = publisher->publish("batch-b", second.data(), second.size());
        ASSERT_TRUE(second_publish.status().ok());
        second_publish.get();
    });

    auto result = waiter->wait_for_keys({"batch-a", "batch-b"}, std::chrono::milliseconds(1000));
    EXPECT_TRUE(result.completed);
    EXPECT_EQ(result.ready.size(), 2u);
    EXPECT_TRUE(result.timed_out.empty());

    publish_thread.join();
    publisher->stop();
    waiter->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, SubscribeAndFetchOnceManyFetchesEachKeyWhenReady) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto waiter = KVNode::create(cfg);
    auto publisher = KVNode::create(cfg);
    ASSERT_TRUE(waiter->start(NodeConfig{server->address(), "127.0.0.1:0", "waiter-d"}).ok());
    ASSERT_TRUE(publisher->start(NodeConfig{server->address(), "127.0.0.1:0", "publisher-d"}).ok());

    std::thread publish_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const std::string first = "value-a";
        auto first_publish = publisher->publish("fetch-a", first.data(), first.size());
        ASSERT_TRUE(first_publish.status().ok());
        first_publish.get();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        const std::string second = "value-b";
        auto second_publish = publisher->publish("fetch-b", second.data(), second.size());
        ASSERT_TRUE(second_publish.status().ok());
        second_publish.get();
    });

    auto result = waiter->subscribe_and_fetch_once_many({"fetch-a", "fetch-b"},
                                                        std::chrono::milliseconds(1000));
    EXPECT_TRUE(result.completed);
    EXPECT_EQ(result.fetched.size(), 2u);
    EXPECT_TRUE(result.failed.empty());
    EXPECT_TRUE(result.timed_out.empty());

    publish_thread.join();
    publisher->stop();
    waiter->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, WaitAndFetchHelpersReturnImmediatelyForExistingKeys) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto waiter = KVNode::create(cfg);
    auto publisher = KVNode::create(cfg);
    ASSERT_TRUE(waiter->start(NodeConfig{server->address(), "127.0.0.1:0", "waiter-existing"}).ok());
    ASSERT_TRUE(publisher->start(NodeConfig{server->address(), "127.0.0.1:0", "publisher-existing"}).ok());

    const std::string value = "already-there";
    auto publish = publisher->publish("existing-key", value.data(), value.size());
    ASSERT_TRUE(publish.status().ok());
    publish.get();

    auto wait_one = waiter->wait_for_key("existing-key", std::chrono::milliseconds(100));
    EXPECT_TRUE(wait_one.ok()) << wait_one.message();

    auto wait_many = waiter->wait_for_keys({"existing-key"}, std::chrono::milliseconds(100));
    EXPECT_TRUE(wait_many.completed);
    ASSERT_EQ(wait_many.ready.size(), 1u);
    EXPECT_EQ(wait_many.ready[0], "existing-key");
    EXPECT_TRUE(wait_many.timed_out.empty());

    auto one = waiter->subscribe_and_fetch_once("existing-key", std::chrono::milliseconds(100));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(one.data.data()), one.data.size()), value);

    auto many = waiter->subscribe_and_fetch_once_many({"existing-key"}, std::chrono::milliseconds(100));
    EXPECT_TRUE(many.completed);
    ASSERT_EQ(many.fetched.size(), 1u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(many.fetched[0].second.data.data()),
                          many.fetched[0].second.data.size()),
              value);
    EXPECT_TRUE(many.failed.empty());
    EXPECT_TRUE(many.timed_out.empty());

    publisher->stop();
    waiter->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, SubscribeAndFetchOnceManyReturnsPartialResultsOnTimeout) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto waiter = KVNode::create(cfg);
    auto publisher = KVNode::create(cfg);
    ASSERT_TRUE(waiter->start(NodeConfig{server->address(), "127.0.0.1:0", "waiter-timeout"}).ok());
    ASSERT_TRUE(publisher->start(NodeConfig{server->address(), "127.0.0.1:0", "publisher-timeout"}).ok());

    std::thread publish_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::string value = "partial-value";
        auto publish = publisher->publish("partial-a", value.data(), value.size());
        ASSERT_TRUE(publish.status().ok());
        publish.get();
    });

    auto result = waiter->subscribe_and_fetch_once_many({"partial-a", "partial-b"},
                                                        std::chrono::milliseconds(200));
    EXPECT_FALSE(result.completed);
    ASSERT_EQ(result.fetched.size(), 1u);
    EXPECT_TRUE(result.failed.empty());
    ASSERT_EQ(result.timed_out.size(), 1u);
    EXPECT_EQ(result.timed_out[0], "partial-b");

    publish_thread.join();
    publisher->stop();
    waiter->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, SubscribeAndFetchOnceManyDeduplicatesKeys) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto waiter = KVNode::create(cfg);
    auto publisher = KVNode::create(cfg);
    ASSERT_TRUE(waiter->start(NodeConfig{server->address(), "127.0.0.1:0", "waiter-dedupe"}).ok());
    ASSERT_TRUE(publisher->start(NodeConfig{server->address(), "127.0.0.1:0", "publisher-dedupe"}).ok());

    const std::string value = "dup-value";
    auto publish = publisher->publish("dup-key", value.data(), value.size());
    ASSERT_TRUE(publish.status().ok());
    publish.get();

    auto result = waiter->subscribe_and_fetch_once_many({"dup-key", "dup-key"},
                                                        std::chrono::milliseconds(500));
    EXPECT_TRUE(result.completed);
    ASSERT_EQ(result.fetched.size(), 1u);
    EXPECT_TRUE(result.failed.empty());
    EXPECT_TRUE(result.timed_out.empty());

    publisher->stop();
    waiter->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, SubscribeAndFetchOnceManyLocksFirstSuccessfulVersion) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto waiter = KVNode::create(cfg);
    auto publisher = KVNode::create(cfg);
    ASSERT_TRUE(waiter->start(NodeConfig{server->address(), "127.0.0.1:0", "waiter-version"}).ok());
    ASSERT_TRUE(publisher->start(NodeConfig{server->address(), "127.0.0.1:0", "publisher-version"}).ok());

    std::thread publish_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const std::string first = "version-one";
        auto first_publish = publisher->publish("versioned-key", first.data(), first.size());
        ASSERT_TRUE(first_publish.status().ok());
        first_publish.get();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const std::string second = "version-two";
        auto second_publish = publisher->publish("versioned-key", second.data(), second.size());
        ASSERT_TRUE(second_publish.status().ok());
        second_publish.get();
    });

    auto result = waiter->subscribe_and_fetch_once_many({"versioned-key"},
                                                        std::chrono::milliseconds(1000));
    ASSERT_EQ(result.fetched.size(), 1u);
    EXPECT_EQ(result.fetched[0].second.version, 1u);
    EXPECT_TRUE(result.completed);

    publish_thread.join();
    publisher->stop();
    waiter->stop();
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
    auto push_metrics = node->last_push_metrics();
    EXPECT_FALSE(publish_metrics.has_value());
    EXPECT_FALSE(fetch_metrics.has_value());
    EXPECT_FALSE(push_metrics.has_value());

    node->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, PushMetricsAreRecordedAndOverwritten) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto sender = KVNode::create(cfg);
    auto target = KVNode::create(cfg);
    ASSERT_TRUE(sender->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "push-metrics-sender",
    }).ok());
    ASSERT_TRUE(target->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "push-metrics-target",
    }).ok());

    const std::string first = "push-metrics-1";
    auto first_push = sender->push("push-metrics-target", "push-metrics-key-1", first.data(), first.size());
    ASSERT_TRUE(first_push.status().ok()) << first_push.status().message();
    first_push.get();

    auto first_metrics = sender->last_push_metrics();
    ASSERT_TRUE(first_metrics.has_value());
    EXPECT_TRUE(first_metrics->ok);
    EXPECT_GT(first_metrics->total_us, 0u);
    EXPECT_GT(first_metrics->get_target_rpc_us, 0u);
    EXPECT_GT(first_metrics->prepare_frame_us, 0u);
    EXPECT_GT(first_metrics->rdma_put_flush_us, 0u);
    EXPECT_GT(first_metrics->commit_rpc_us, 0u);

    const std::string second = "push-metrics-2";
    auto second_push = sender->push("push-metrics-target", "push-metrics-key-2", second.data(), second.size());
    ASSERT_TRUE(second_push.status().ok()) << second_push.status().message();
    second_push.get();

    auto second_metrics = sender->last_push_metrics();
    ASSERT_TRUE(second_metrics.has_value());
    EXPECT_TRUE(second_metrics->ok);
    EXPECT_GT(second_metrics->total_us, 0u);
    EXPECT_GT(second_metrics->get_target_rpc_us, 0u);
    EXPECT_GT(second_metrics->prepare_frame_us, 0u);
    EXPECT_GT(second_metrics->rdma_put_flush_us, 0u);
    EXPECT_GT(second_metrics->commit_rpc_us, 0u);

    target->stop();
    sender->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, PushFailureRecordsPushMetrics) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto sender = KVNode::create(cfg);
    ASSERT_TRUE(sender->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "push-metrics-missing",
    }).ok());

    const std::string value = "push-missing-target";
    auto push = sender->push("no-such-target-for-metrics", "push-metrics-key-missing", value.data(), value.size());
    EXPECT_FALSE(push.status().ok());

    auto metrics = sender->last_push_metrics();
    ASSERT_TRUE(metrics.has_value());
    EXPECT_FALSE(metrics->ok);
    EXPECT_GT(metrics->total_us, 0u);
    EXPECT_GT(metrics->get_target_rpc_us, 0u);
    EXPECT_EQ(metrics->prepare_frame_us, 0u);
    EXPECT_EQ(metrics->rdma_put_flush_us, 0u);
    EXPECT_EQ(metrics->commit_rpc_us, 0u);

    sender->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, SubscriptionQueueIsEmptyBeforeUse) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto node = KVNode::create(cfg);
    ASSERT_NE(node, nullptr);
    ASSERT_TRUE(node->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:20017",
        .node_id = "subscription-empty-node",
    }).ok());

    auto subscribe = node->subscribe("alpha");
    auto unsubscribe = node->unsubscribe("alpha");
    auto events = node->drain_subscription_events();

    EXPECT_TRUE(subscribe.status().ok());
    EXPECT_TRUE(unsubscribe.status().ok());
    EXPECT_TRUE(events.empty());

    node->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, SubscriptionReceivesPublishedAndUpdatedEvents) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto publisher = KVNode::create(cfg);
    auto subscriber = KVNode::create(cfg);
    ASSERT_TRUE(publisher->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:26001",
        .node_id = "sub-publisher-a",
    }).ok());
    ASSERT_TRUE(subscriber->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:26002",
        .node_id = "sub-subscriber-a",
    }).ok());

    auto subscribe = subscriber->subscribe("alpha");
    ASSERT_TRUE(subscribe.status().ok());
    subscribe.get();

    const std::string first = "value-one";
    auto publish = publisher->publish("alpha", first.data(), first.size());
    ASSERT_TRUE(publish.status().ok());
    publish.get();

    const std::string second = "value-two";
    auto update = publisher->publish("alpha", second.data(), second.size());
    ASSERT_TRUE(update.status().ok());
    update.get();

    auto events = drain_until_count(subscriber, 2);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].type, axon::kv::SubscriptionEventType::kPublished);
    EXPECT_EQ(events[0].key, "alpha");
    EXPECT_EQ(events[0].owner_node_id, "sub-publisher-a");
    EXPECT_EQ(events[0].version, 1u);
    EXPECT_EQ(events[1].type, axon::kv::SubscriptionEventType::kUpdated);
    EXPECT_EQ(events[1].version, 2u);

    subscriber->stop();
    publisher->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, SubscriptionReceivesUnpublishedEvent) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto publisher = KVNode::create(cfg);
    auto subscriber = KVNode::create(cfg);
    ASSERT_TRUE(publisher->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:26011",
        .node_id = "sub-publisher-b",
    }).ok());
    ASSERT_TRUE(subscriber->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:26012",
        .node_id = "sub-subscriber-b",
    }).ok());

    auto subscribe = subscriber->subscribe("beta");
    ASSERT_TRUE(subscribe.status().ok());
    subscribe.get();

    const std::string value = "value-three";
    auto publish = publisher->publish("beta", value.data(), value.size());
    ASSERT_TRUE(publish.status().ok());
    publish.get();
    auto unpublish = publisher->unpublish("beta");
    ASSERT_TRUE(unpublish.status().ok());
    unpublish.get();

    auto events = drain_until_count(subscriber, 2);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].type, axon::kv::SubscriptionEventType::kPublished);
    EXPECT_EQ(events[1].type, axon::kv::SubscriptionEventType::kUnpublished);
    EXPECT_EQ(events[1].key, "beta");
    EXPECT_EQ(events[1].owner_node_id, "sub-publisher-b");

    subscriber->stop();
    publisher->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, SubscriptionReceivesOwnerLostEvent) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto publisher = KVNode::create(cfg);
    auto subscriber = KVNode::create(cfg);
    ASSERT_TRUE(publisher->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:26021",
        .node_id = "sub-publisher-c",
    }).ok());
    ASSERT_TRUE(subscriber->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:26022",
        .node_id = "sub-subscriber-c",
    }).ok());

    auto subscribe = subscriber->subscribe("gamma");
    ASSERT_TRUE(subscribe.status().ok());
    subscribe.get();

    const std::string value = "value-four";
    auto publish = publisher->publish("gamma", value.data(), value.size());
    ASSERT_TRUE(publish.status().ok());
    publish.get();

    publisher->stop();

    auto events = drain_until_count(subscriber, 2);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].type, axon::kv::SubscriptionEventType::kPublished);
    EXPECT_EQ(events[1].type, axon::kv::SubscriptionEventType::kOwnerLost);
    EXPECT_EQ(events[1].key, "gamma");
    EXPECT_EQ(events[1].owner_node_id, "sub-publisher-c");

    subscriber->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, UnsubscribeStopsFutureSubscriptionEvents) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto publisher = KVNode::create(cfg);
    auto subscriber = KVNode::create(cfg);
    ASSERT_TRUE(publisher->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:26031",
        .node_id = "sub-publisher-d",
    }).ok());
    ASSERT_TRUE(subscriber->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:26032",
        .node_id = "sub-subscriber-d",
    }).ok());

    auto subscribe = subscriber->subscribe("delta");
    ASSERT_TRUE(subscribe.status().ok());
    subscribe.get();

    const std::string first = "value-five";
    auto publish = publisher->publish("delta", first.data(), first.size());
    ASSERT_TRUE(publish.status().ok());
    publish.get();

    auto first_events = drain_until_count(subscriber, 1);
    ASSERT_EQ(first_events.size(), 1u);
    EXPECT_EQ(first_events[0].type, axon::kv::SubscriptionEventType::kPublished);

    auto unsubscribe = subscriber->unsubscribe("delta");
    ASSERT_TRUE(unsubscribe.status().ok());
    unsubscribe.get();

    const std::string second = "value-six";
    auto update = publisher->publish("delta", second.data(), second.size());
    ASSERT_TRUE(update.status().ok());
    update.get();

    auto events = drain_until_count(subscriber, 1, std::chrono::milliseconds(250));
    EXPECT_TRUE(events.empty());

    subscriber->stop();
    publisher->stop();
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
    EXPECT_GE(metrics->rkey_prepare_us, 0u);
    EXPECT_GE(metrics->get_submit_us, 0u);
    EXPECT_GE(metrics->rdma_prepare_us, metrics->rkey_prepare_us + metrics->get_submit_us);
    EXPECT_GT(metrics->rdma_get_us, 0u);
    EXPECT_GT(metrics->result_copy_us, 0u);

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
    EXPECT_GE(metrics->rkey_prepare_us, 0u);
    EXPECT_GE(metrics->get_submit_us, 0u);
    EXPECT_GE(metrics->rdma_prepare_us, metrics->rkey_prepare_us + metrics->get_submit_us);
    EXPECT_GT(metrics->rdma_get_us, 0u);
    EXPECT_EQ(metrics->result_copy_us, 0u);

    reader->stop();
    publisher->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, FetchToManyWritesMultipleKeysIntoSharedRegion) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto publisher = KVNode::create(cfg);
    auto reader = KVNode::create(cfg);
    ASSERT_TRUE(publisher->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:22031",
        .node_id = "publisher-fetch-to-many",
    }).ok());
    ASSERT_TRUE(reader->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:22032",
        .node_id = "reader-fetch-to-many",
    }).ok());

    const std::string value_a = "alpha-many";
    const std::string value_b = "bravo-many";
    auto publish_a = publisher->publish("many-a", value_a.data(), value_a.size());
    auto publish_b = publisher->publish("many-b", value_b.data(), value_b.size());
    ASSERT_TRUE(publish_a.status().ok());
    ASSERT_TRUE(publish_b.status().ok());
    publish_a.get();
    publish_b.get();

    auto ctx = axon::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = axon::MemoryRegion::allocate(ctx, 128);
    ASSERT_NE(region, nullptr);

    auto result = reader->fetch_to_many({
        {.key = "many-a", .length = value_a.size(), .offset = 0},
        {.key = "many-b", .length = value_b.size(), .offset = 32},
    }, region);

    EXPECT_TRUE(result.all_succeeded);
    EXPECT_EQ(result.completed.size(), 2u);
    EXPECT_TRUE(result.failed.empty());

    const auto* bytes = static_cast<const char*>(region->address());
    EXPECT_EQ(std::memcmp(bytes, value_a.data(), value_a.size()), 0);
    EXPECT_EQ(std::memcmp(bytes + 32, value_b.data(), value_b.size()), 0);

    reader->stop();
    publisher->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, FetchToManyReturnsPartialProgressOnPerKeyFailure) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto publisher = KVNode::create(cfg);
    auto reader = KVNode::create(cfg);
    ASSERT_TRUE(publisher->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:22033",
        .node_id = "publisher-fetch-to-many-fail",
    }).ok());
    ASSERT_TRUE(reader->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:22034",
        .node_id = "reader-fetch-to-many-fail",
    }).ok());

    const std::string value = "present-value";
    auto publish = publisher->publish("present-key", value.data(), value.size());
    ASSERT_TRUE(publish.status().ok());
    publish.get();

    auto ctx = axon::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = axon::MemoryRegion::allocate(ctx, 128);
    ASSERT_NE(region, nullptr);

    auto result = reader->fetch_to_many({
        {.key = "present-key", .length = value.size(), .offset = 0},
        {.key = "missing-key", .length = 16, .offset = 32},
    }, region);

    EXPECT_FALSE(result.all_succeeded);
    EXPECT_EQ(result.completed.size(), 1u);
    EXPECT_EQ(result.failed.size(), 1u);
    EXPECT_EQ(result.completed[0], "present-key");
    EXPECT_EQ(result.failed[0], "missing-key");
    EXPECT_EQ(std::memcmp(static_cast<const char*>(region->address()), value.data(), value.size()), 0);

    reader->stop();
    publisher->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, FetchToManyRejectsOverlappingRanges) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();
    auto node = KVNode::create(cfg);
    auto ctx = axon::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = axon::MemoryRegion::allocate(ctx, 64);
    ASSERT_NE(region, nullptr);

    EXPECT_THROW((void)node->fetch_to_many({
        {.key = "overlap-a", .length = 16, .offset = 0},
        {.key = "overlap-b", .length = 16, .offset = 8},
    }, region), std::system_error);
}

TEST(KvNodeIntegrationTest, FetchToManyRejectsOutOfBoundsRanges) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();
    auto node = KVNode::create(cfg);
    auto ctx = axon::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = axon::MemoryRegion::allocate(ctx, 32);
    ASSERT_NE(region, nullptr);

    EXPECT_THROW((void)node->fetch_to_many({
        {.key = "oob-key", .length = 16, .offset = 24},
    }, region), std::system_error);
}

TEST(KvNodeIntegrationTest, FetchToManyAllowsDuplicateKeysAtDistinctOffsets) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto publisher = KVNode::create(cfg);
    auto reader = KVNode::create(cfg);
    ASSERT_TRUE(publisher->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:22035",
        .node_id = "publisher-fetch-to-many-dup",
    }).ok());
    ASSERT_TRUE(reader->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:22036",
        .node_id = "reader-fetch-to-many-dup",
    }).ok());

    const std::string value = "dup-value";
    auto publish = publisher->publish("dup-key", value.data(), value.size());
    ASSERT_TRUE(publish.status().ok());
    publish.get();

    auto ctx = axon::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = axon::MemoryRegion::allocate(ctx, 128);
    ASSERT_NE(region, nullptr);

    auto result = reader->fetch_to_many({
        {.key = "dup-key", .length = value.size(), .offset = 0},
        {.key = "dup-key", .length = value.size(), .offset = 32},
    }, region);

    EXPECT_TRUE(result.all_succeeded);
    EXPECT_EQ(result.completed.size(), 2u);
    EXPECT_TRUE(result.failed.empty());
    const auto* bytes = static_cast<const char*>(region->address());
    EXPECT_EQ(std::memcmp(bytes, value.data(), value.size()), 0);
    EXPECT_EQ(std::memcmp(bytes + 32, value.data(), value.size()), 0);

    reader->stop();
    publisher->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, FetchToManyRejectsEmptyItems) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();
    auto node = KVNode::create(cfg);
    auto ctx = axon::Context::create(cfg);
    ASSERT_NE(ctx, nullptr);
    auto region = axon::MemoryRegion::allocate(ctx, 32);
    ASSERT_NE(region, nullptr);

    EXPECT_THROW((void)node->fetch_to_many({}, region), std::system_error);
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

TEST(KvNodeIntegrationTest, PushPublishesOnTarget) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto sender = KVNode::create(cfg);
    auto target = KVNode::create(cfg);
    auto reader = KVNode::create(cfg);
    ASSERT_NE(sender, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_NE(reader, nullptr);

    ASSERT_TRUE(sender->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "push-sender",
    }).ok());
    ASSERT_TRUE(target->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "push-target",
    }).ok());
    ASSERT_TRUE(reader->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "push-reader",
    }).ok());

    const std::string value = "rdma-pushed-value";
    auto push = sender->push("push-target", "pushed-key", value.data(), value.size());
    ASSERT_TRUE(push.status().ok()) << push.status().message();
    push.get();

    auto fetched = reader->fetch("pushed-key");
    ASSERT_TRUE(fetched.status().ok());
    auto result = fetched.get();

    EXPECT_EQ(result.owner_node_id, "push-target");
    EXPECT_EQ(result.version, 1u);
    ASSERT_EQ(result.data.size(), value.size());
    EXPECT_EQ(std::memcmp(result.data.data(), value.data(), value.size()), 0);

    reader->stop();
    target->stop();
    sender->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, PushFailsWhenTargetNodeIsUnknown) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto sender = KVNode::create(cfg);
    ASSERT_NE(sender, nullptr);
    ASSERT_TRUE(sender->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "push-sender-missing",
    }).ok());

    const std::string value = "missing-target";
    auto push = sender->push("no-such-node", "missing-key", value.data(), value.size());
    EXPECT_FALSE(push.status().ok()) << "push unexpectedly succeeded";

    sender->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, PushFailsWhenPayloadExceedsInboxCapacity) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto sender = KVNode::create(cfg);
    auto target = KVNode::create(cfg);
    ASSERT_NE(sender, nullptr);
    ASSERT_NE(target, nullptr);

    ASSERT_TRUE(sender->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "push-sender-big",
    }).ok());
    ASSERT_TRUE(target->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "push-target-big",
    }).ok());

    std::string big_value(70 * 1024, 'x');
    auto push = sender->push("push-target-big", "too-big-key", big_value.data(), big_value.size());
    EXPECT_FALSE(push.status().ok()) << "push unexpectedly succeeded";

    target->stop();
    sender->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, PushCoexistsWithPublish) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto sender = KVNode::create(cfg);
    auto target = KVNode::create(cfg);
    auto reader = KVNode::create(cfg);
    ASSERT_NE(sender, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_NE(reader, nullptr);

    ASSERT_TRUE(sender->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "push-sender-coexist",
    }).ok());
    ASSERT_TRUE(target->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "push-target-coexist",
    }).ok());
    ASSERT_TRUE(reader->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "push-reader-coexist",
    }).ok());

    const std::string pushed = "pushed-value";
    auto push = sender->push("push-target-coexist", "pushed-key-2", pushed.data(), pushed.size());
    ASSERT_TRUE(push.status().ok()) << push.status().message();
    push.get();

    const std::string local = "published-value";
    auto publish = target->publish("published-key-2", local.data(), local.size());
    ASSERT_TRUE(publish.status().ok());
    publish.get();

    auto fetched_pushed = reader->fetch("pushed-key-2");
    ASSERT_TRUE(fetched_pushed.status().ok());
    auto pushed_result = fetched_pushed.get();
    ASSERT_EQ(pushed_result.data.size(), pushed.size());
    EXPECT_EQ(std::memcmp(pushed_result.data.data(), pushed.data(), pushed.size()), 0);

    auto fetched_local = reader->fetch("published-key-2");
    ASSERT_TRUE(fetched_local.status().ok());
    auto local_result = fetched_local.get();
    ASSERT_EQ(local_result.data.size(), local.size());
    EXPECT_EQ(std::memcmp(local_result.data.data(), local.data(), local.size()), 0);

    reader->stop();
    target->stop();
    sender->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, PushFailsWhileTargetInboxReservationIsHeld) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto sender_a = KVNode::create(cfg);
    auto target = KVNode::create(cfg);
    auto reader = KVNode::create(cfg);
    ASSERT_NE(sender_a, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_NE(reader, nullptr);

    ASSERT_TRUE(sender_a->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "push-sender-a",
    }).ok());
    ASSERT_TRUE(target->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "push-target-race",
    }).ok());
    ASSERT_TRUE(reader->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "push-reader-race",
    }).ok());

    auto target_info = get_push_target(server->address(), "push-target-race");
    ASSERT_TRUE(target_info.has_value());
    ASSERT_EQ(target_info->status, proto::MsgStatus::kOk);

    std::string error;
    int reserve_fd = proto::TcpTransport::connect(target_info->push_control_addr, &error);
    ASSERT_GE(reserve_fd, 0) << error;

    proto::ReservePushInboxRequest reserve;
    reserve.target_node_id = "push-target-race";
    reserve.sender_node_id = "manual-reserver";
    reserve.key = "held-key";
    reserve.value_size = 4;
    ASSERT_TRUE(proto::send_frame(reserve_fd, proto::MsgType::kReservePushInbox, 1, proto::encode(reserve)));

    proto::MsgHeader reserve_header;
    std::vector<uint8_t> reserve_payload;
    ASSERT_TRUE(proto::recv_frame(reserve_fd, &reserve_header, &reserve_payload));
    auto reserve_resp = proto::decode_reserve_push_inbox_response(reserve_payload);
    ASSERT_TRUE(reserve_resp.has_value());
    ASSERT_EQ(reserve_resp->status, proto::MsgStatus::kOk);

    const std::string value = "value-from-a";
    auto blocked = sender_a->push("push-target-race", "race-key-a", value.data(), value.size());
    EXPECT_FALSE(blocked.status().ok());

    proto::TcpTransport::close_fd(&reserve_fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto push = sender_a->push("push-target-race", "race-key-a", value.data(), value.size());
    ASSERT_TRUE(push.status().ok()) << push.status().message();
    push.get();

    auto fetched = reader->fetch("race-key-a");
    ASSERT_TRUE(fetched.status().ok());
    auto result = fetched.get();
    ASSERT_EQ(result.data.size(), value.size());
    EXPECT_EQ(std::memcmp(result.data.data(), value.data(), value.size()), 0);

    reader->stop();
    target->stop();
    sender_a->stop();
    server->stop();
}

}  // namespace
