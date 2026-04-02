#pragma once

/// @file zerokv/core/kv_node.h
/// @brief KV node infrastructure API.

#include "zerokv/config.h"
#include "zerokv/core/common_types.h"
#include "zerokv/future.h"
#include "zerokv/memory.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zerokv::kv {

class KVNode {
public:
    using Ptr = std::shared_ptr<KVNode>;

    static Ptr create(const zerokv::Config& cfg = {});

    ~KVNode();
    KVNode(const KVNode&) = delete;
    KVNode& operator=(const KVNode&) = delete;

    zerokv::Status start(const NodeConfig& cfg);
    void stop();

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] std::string node_id() const;
    [[nodiscard]] zerokv::MemoryRegion::Ptr allocate_region(size_t size) const;
    [[nodiscard]] size_t published_count() const noexcept;
    [[nodiscard]] std::optional<PublishMetrics> last_publish_metrics() const;
    [[nodiscard]] std::optional<FetchMetrics> last_fetch_metrics() const;
    [[nodiscard]] std::optional<PushMetrics> last_push_metrics() const;
    [[nodiscard]] std::vector<SubscriptionEvent> drain_subscription_events();
    zerokv::Status wait_for_subscription_event(const std::string& key,
                                               std::chrono::milliseconds timeout);
    std::optional<SubscriptionEvent> wait_for_any_subscription_event(
        const std::vector<std::string>& keys,
        std::chrono::milliseconds timeout);
    zerokv::Status wait_for_key(const std::string& key,
                                std::chrono::milliseconds timeout);
    WaitKeysResult wait_for_keys(const std::vector<std::string>& keys,
                                 std::chrono::milliseconds timeout);
    FetchResult subscribe_and_fetch_once(const std::string& key,
                                         std::chrono::milliseconds timeout);
    BatchFetchResult subscribe_and_fetch_once_many(const std::vector<std::string>& keys,
                                                   std::chrono::milliseconds timeout);

    zerokv::Future<void> publish(const std::string& key,
                                 const void* data,
                                 size_t size);
    zerokv::Future<void> publish_region(const std::string& key,
                                        const zerokv::MemoryRegion::Ptr& region,
                                        size_t size);
    zerokv::Future<FetchResult> fetch(const std::string& key);
    zerokv::Future<void> fetch_to(const std::string& key,
                                  const zerokv::MemoryRegion::Ptr& local_region,
                                  size_t length,
                                  size_t local_offset = 0);
    FetchToManyResult fetch_to_many(const std::vector<FetchToItem>& items,
                                    const zerokv::MemoryRegion::Ptr& local_region);
    BatchFetchToResult subscribe_and_fetch_to_once_many(
        const std::vector<FetchToItem>& items,
        const zerokv::MemoryRegion::Ptr& local_region,
        std::chrono::milliseconds timeout);

    zerokv::Future<void> push(const std::string& target_node_id,
                              const std::string& key,
                              const void* data,
                              size_t size);
    zerokv::Future<void> subscribe(const std::string& key);
    zerokv::Future<void> unsubscribe(const std::string& key);
    zerokv::Future<void> unpublish(const std::string& key);

private:
    explicit KVNode(const zerokv::Config& cfg);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zerokv::kv

namespace zerokv {
namespace core {

using KVNode = ::zerokv::kv::KVNode;

}  // namespace core
}  // namespace zerokv
