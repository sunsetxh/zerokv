#pragma once

/// @file axon/kv.h
/// @brief High-level RDMA KV MVP API built on top of axon transport primitives.

#include "axon/common.h"
#include "axon/config.h"
#include "axon/future.h"
#include "axon/memory.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace axon::kv {

struct KeyInfo {
    std::string key;
    size_t size = 0;
    uint64_t version = 0;
};

struct FetchResult {
    std::vector<std::byte> data;
    std::string owner_node_id;
    uint64_t version = 0;
};

struct WaitKeysResult {
    std::vector<std::string> ready;
    std::vector<std::string> timed_out;
    bool completed = false;
};

struct BatchFetchResult {
    std::vector<std::pair<std::string, FetchResult>> fetched;
    std::vector<std::string> failed;
    std::vector<std::string> timed_out;
    bool completed = false;
};

struct PublishMetrics {
    uint64_t total_us = 0;
    uint64_t prepare_region_us = 0;
    uint64_t pack_rkey_us = 0;
    uint64_t put_meta_rpc_us = 0;
    bool ok = false;
};

struct FetchMetrics {
    uint64_t total_us = 0;
    uint64_t local_buffer_prepare_us = 0;
    uint64_t get_meta_rpc_us = 0;
    uint64_t peer_connect_us = 0;
    uint64_t rkey_prepare_us = 0;
    uint64_t get_submit_us = 0;
    uint64_t rdma_prepare_us = 0;
    uint64_t rdma_get_us = 0;
    uint64_t result_copy_us = 0;
    bool ok = false;
};

struct PushMetrics {
    uint64_t total_us = 0;
    uint64_t get_target_rpc_us = 0;
    uint64_t prepare_frame_us = 0;
    uint64_t rdma_put_flush_us = 0;
    uint64_t commit_rpc_us = 0;
    bool ok = false;
};

enum class SubscriptionEventType {
    kPublished,
    kUpdated,
    kUnpublished,
    kOwnerLost,
};

struct SubscriptionEvent {
    SubscriptionEventType type = SubscriptionEventType::kPublished;
    std::string key;
    std::string owner_node_id;
    uint64_t version = 0;
};

struct ServerConfig {
    std::string listen_addr;
};

struct NodeConfig {
    std::string server_addr;
    std::string local_data_addr;
    std::string node_id;
};

class KVServer {
public:
    using Ptr = std::shared_ptr<KVServer>;

    static Ptr create(const axon::Config& cfg = {});

    ~KVServer();
    KVServer(const KVServer&) = delete;
    KVServer& operator=(const KVServer&) = delete;

    axon::Status start(const ServerConfig& cfg);
    void stop();

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] std::string address() const;
    [[nodiscard]] std::optional<KeyInfo> lookup(const std::string& key) const;
    [[nodiscard]] std::vector<std::string> list_keys() const;

private:
    explicit KVServer(const axon::Config& cfg);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class KVNode {
public:
    using Ptr = std::shared_ptr<KVNode>;

    static Ptr create(const axon::Config& cfg = {});

    ~KVNode();
    KVNode(const KVNode&) = delete;
    KVNode& operator=(const KVNode&) = delete;

    axon::Status start(const NodeConfig& cfg);
    void stop();

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] std::string node_id() const;
    [[nodiscard]] size_t published_count() const noexcept;
    [[nodiscard]] std::optional<PublishMetrics> last_publish_metrics() const;
    [[nodiscard]] std::optional<FetchMetrics> last_fetch_metrics() const;
    [[nodiscard]] std::optional<PushMetrics> last_push_metrics() const;
    [[nodiscard]] std::vector<SubscriptionEvent> drain_subscription_events();
    axon::Status wait_for_key(const std::string& key,
                              std::chrono::milliseconds timeout);
    WaitKeysResult wait_for_keys(const std::vector<std::string>& keys,
                                 std::chrono::milliseconds timeout);
    FetchResult subscribe_and_fetch_once(const std::string& key,
                                         std::chrono::milliseconds timeout);
    BatchFetchResult subscribe_and_fetch_once_many(const std::vector<std::string>& keys,
                                                   std::chrono::milliseconds timeout);

    /// Copy-publish semantics: the implementation owns the data after the
    /// returned future completes, so the caller may release the input buffer.
    axon::Future<void> publish(const std::string& key,
                               const void* data,
                               size_t size);

    /// Zero-copy publish semantics: the caller must keep the region alive
    /// until the key is unpublished or the node stops.
    axon::Future<void> publish_region(const std::string& key,
                                      const axon::MemoryRegion::Ptr& region,
                                      size_t size);

    /// Convenience API that allocates a local buffer and fetches into it.
    axon::Future<FetchResult> fetch(const std::string& key);

    /// Primary zero-copy fetch API.
    axon::Future<void> fetch_to(const std::string& key,
                                const axon::MemoryRegion::Ptr& local_region,
                                size_t length,
                                size_t local_offset = 0);

    axon::Future<void> push(const std::string& target_node_id,
                            const std::string& key,
                            const void* data,
                            size_t size);

    axon::Future<void> subscribe(const std::string& key);
    axon::Future<void> unsubscribe(const std::string& key);

    axon::Future<void> unpublish(const std::string& key);

private:
    explicit KVNode(const axon::Config& cfg);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace axon::kv
