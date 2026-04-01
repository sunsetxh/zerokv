#include "zerokv/message_kv.h"

#include <chrono>
#include <mutex>
#include <utility>

namespace zerokv {

namespace {

constexpr const char* kAckPrefix = "__message_kv_ack__:";
constexpr char kAckMarker = '1';

std::string make_ack_key(const std::string& message_key) {
    return std::string(kAckPrefix) + message_key;
}

}  // namespace

struct MessageKV::Impl {
    explicit Impl(const zerokv::Config& config) : cfg(config) {}

    zerokv::Config cfg;
    zerokv::kv::KVNode::Ptr node;
    std::mutex mu;
    bool running = false;
    std::vector<std::string> owned_ack_keys;

    bool node_ready_locked() const noexcept {
        return running && static_cast<bool>(node);
    }

    void sweep_receiver_ack_cleanup_locked() {
        if (!node_ready_locked() || owned_ack_keys.empty()) {
            return;
        }

        std::vector<std::string> remaining;
        remaining.reserve(owned_ack_keys.size());
        for (const auto& ack_key : owned_ack_keys) {
            auto unpublish = node->unpublish(ack_key);
            unpublish.get();
            if (!unpublish.status().ok()) {
                remaining.push_back(ack_key);
            }
        }

        owned_ack_keys.swap(remaining);
    }

    void sweep_cleanup_locked() {
        sweep_receiver_ack_cleanup_locked();
    }

    void publish_ack_locked(const std::string& message_key) {
        const auto ack_key = make_ack_key(message_key);
        auto publish = node->publish(ack_key, &kAckMarker, sizeof(kAckMarker));
        publish.get();
        publish.status().throw_if_error();
    }

    void record_owned_ack_keys_locked(std::vector<std::string> ack_keys) {
        owned_ack_keys.reserve(owned_ack_keys.size() + ack_keys.size());
        for (auto& ack_key : ack_keys) {
            owned_ack_keys.push_back(std::move(ack_key));
        }
    }

    void wait_for_ack_and_cleanup_message_locked(const std::string& message_key) {
        const auto ack_key = make_ack_key(message_key);
        const auto ack_status = node->wait_for_key(ack_key, cfg.connect_timeout());
        if (!ack_status.ok()) {
            auto rollback = node->unpublish(message_key);
            rollback.get();
            ack_status.throw_if_error();
        }

        auto unpublish = node->unpublish(message_key);
        unpublish.get();
        unpublish.status().throw_if_error();
    }
};

MessageKV::MessageKV(const zerokv::Config& cfg) : impl_(std::make_unique<Impl>(cfg)) {}
MessageKV::~MessageKV() = default;

MessageKV::Ptr MessageKV::create(const zerokv::Config& cfg) {
    return Ptr(new MessageKV(cfg));
}

void MessageKV::start(const zerokv::kv::NodeConfig& cfg) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->running) {
        return;
    }

    impl_->node = zerokv::kv::KVNode::create(impl_->cfg);
    auto status = impl_->node->start(cfg);
    status.throw_if_error();
    impl_->running = true;
}

void MessageKV::stop() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->running) {
        return;
    }

    impl_->sweep_cleanup_locked();
    impl_->node->stop();
    impl_->node.reset();
    impl_->running = false;
    impl_->owned_ack_keys.clear();
}

void MessageKV::send(const std::string& key, const void* data, size_t size) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->sweep_cleanup_locked();
    if (key.empty()) {
        throw std::system_error(make_error_code(ErrorCode::kInvalidArgument));
    }
    if (size > 0 && data == nullptr) {
        throw std::system_error(make_error_code(ErrorCode::kInvalidArgument));
    }
    if (!impl_->node_ready_locked()) {
        throw std::system_error(make_error_code(ErrorCode::kConnectionRefused));
    }

    auto publish = impl_->node->publish(key, data, size);
    publish.get();
    publish.status().throw_if_error();
    impl_->wait_for_ack_and_cleanup_message_locked(key);
}

void MessageKV::send_region(const std::string& key,
                            const zerokv::MemoryRegion::Ptr& region,
                            size_t size) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->sweep_cleanup_locked();
    if (key.empty()) {
        throw std::system_error(make_error_code(ErrorCode::kInvalidArgument));
    }
    if (!region) {
        throw std::system_error(make_error_code(ErrorCode::kInvalidArgument));
    }
    if (size > region->length()) {
        throw std::system_error(make_error_code(ErrorCode::kInvalidArgument));
    }
    if (!impl_->node_ready_locked()) {
        throw std::system_error(make_error_code(ErrorCode::kConnectionRefused));
    }

    auto publish = impl_->node->publish_region(key, region, size);
    publish.get();
    publish.status().throw_if_error();
    impl_->wait_for_ack_and_cleanup_message_locked(key);
}

void MessageKV::recv(const std::string& key,
                     const zerokv::MemoryRegion::Ptr& region,
                     size_t length,
                     size_t offset,
                     std::chrono::milliseconds timeout) {
    if (key.empty()) {
        throw std::system_error(make_error_code(ErrorCode::kInvalidArgument));
    }
    auto result = recv_batch({BatchRecvItem{key, length, offset}}, region, timeout);
    if (!result.completed_all || !result.failed.empty() || !result.timed_out.empty()) {
        throw std::system_error(make_error_code(ErrorCode::kTimeout));
    }
}

MessageKV::BatchRecvResult MessageKV::recv_batch(const std::vector<BatchRecvItem>& items,
                                                 const zerokv::MemoryRegion::Ptr& region,
                                                 std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->sweep_cleanup_locked();
    if (!impl_->node_ready_locked()) {
        throw std::system_error(make_error_code(ErrorCode::kConnectionRefused));
    }

    std::vector<zerokv::kv::FetchToItem> kv_items;
    kv_items.reserve(items.size());
    for (const auto& item : items) {
        kv_items.push_back(zerokv::kv::FetchToItem{
            .key = item.key,
            .length = item.length,
            .offset = item.offset,
        });
    }

    auto batch = impl_->node->subscribe_and_fetch_to_once_many(kv_items, region, timeout);

    std::vector<std::string> new_ack_keys;
    new_ack_keys.reserve(batch.completed.size());
    for (const auto& key : batch.completed) {
        impl_->publish_ack_locked(key);
        new_ack_keys.push_back(make_ack_key(key));
    }

    BatchRecvResult result;
    result.completed = std::move(batch.completed);
    result.failed = std::move(batch.failed);
    result.timed_out = std::move(batch.timed_out);
    result.completed_all = batch.completed_all;
    impl_->sweep_cleanup_locked();
    impl_->record_owned_ack_keys_locked(std::move(new_ack_keys));
    return result;
}

}  // namespace zerokv
