#include "zerokv/message_kv.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace zerokv {

namespace {

constexpr const char* kAckPrefix = "__message_kv_ack__:";
constexpr char kAckMarker = '1';

using SteadyClock = std::chrono::steady_clock;

uint64_t elapsed_us(SteadyClock::time_point start, SteadyClock::time_point end) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

bool message_kv_trace_enabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("ZEROKV_MESSAGE_KV_TRACE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

void trace_message_kv(const std::string& line) {
    if (message_kv_trace_enabled()) {
        std::cerr << line << "\n";
    }
}

std::string make_ack_key(const std::string& message_key) {
    return std::string(kAckPrefix) + message_key;
}

struct RecvRange {
    size_t start = 0;
    size_t end = 0;
};

void validate_recv_batch_layout(const std::vector<MessageKV::BatchRecvItem>& items,
                                const zerokv::MemoryRegion::Ptr& region) {
    if (!region) {
        Status(ErrorCode::kInvalidArgument, "local region is required").throw_if_error();
    }
    if (items.empty()) {
        Status(ErrorCode::kInvalidArgument, "at least one receive item is required").throw_if_error();
    }

    std::vector<RecvRange> ranges;
    ranges.reserve(items.size());
    for (const auto& item : items) {
        if (item.key.empty()) {
            Status(ErrorCode::kInvalidArgument, "key is required").throw_if_error();
        }
        if (item.length == 0) {
            Status(ErrorCode::kInvalidArgument, "receive length must be greater than zero")
                .throw_if_error();
        }
        if (item.offset > region->length() || item.length > region->length() - item.offset) {
            Status(ErrorCode::kInvalidArgument, "receive range is out of bounds").throw_if_error();
        }
        ranges.push_back(RecvRange{
            .start = item.offset,
            .end = item.offset + item.length,
        });
    }

    std::sort(ranges.begin(), ranges.end(), [](const RecvRange& lhs, const RecvRange& rhs) {
        if (lhs.start != rhs.start) {
            return lhs.start < rhs.start;
        }
        return lhs.end < rhs.end;
    });
    for (size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].start < ranges[i - 1].end) {
            Status(ErrorCode::kInvalidArgument, "receive ranges must not overlap").throw_if_error();
        }
    }
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
        const auto total_start = SteadyClock::now();
        auto subscribe_future = node->subscribe(ack_key);
        subscribe_future.status().throw_if_error();
        subscribe_future.get();
        const auto subscribe_end = SteadyClock::now();
        auto ack_status = Status(ErrorCode::kTimeout, "waiting for ack");
        uint64_t event_wait_us = 0;
        const auto event_wait_start = SteadyClock::now();
        ack_status = node->wait_for_subscription_event(ack_key, cfg.connect_timeout());
        const auto event_wait_end = SteadyClock::now();
        event_wait_us = elapsed_us(event_wait_start, event_wait_end);
        if (!ack_status.ok()) {
            auto fallback = node->wait_for_key(ack_key, std::chrono::milliseconds(1));
            if (fallback.ok()) {
                ack_status = Status::OK();
            }
        }
        const auto wait_end = SteadyClock::now();
        trace_message_kv("MESSAGE_KV_ACK_WAIT key=" + message_key +
                         " subscribe_us=" + std::to_string(elapsed_us(total_start, subscribe_end)) +
                         " fast_check_us=0" +
                         " event_wait_us=" + std::to_string(event_wait_us) +
                         " total_wait_us=" + std::to_string(elapsed_us(subscribe_end, wait_end)) +
                         " status=" + std::to_string(static_cast<int>(ack_status.code())));

        const auto unsubscribe_start = SteadyClock::now();
        auto unsubscribe_future = node->unsubscribe(ack_key);
        if (unsubscribe_future.status().ok()) {
            unsubscribe_future.get();
        }
        const auto unsubscribe_end = SteadyClock::now();

        if (!ack_status.ok()) {
            auto rollback = node->unpublish(message_key);
            rollback.get();
            ack_status.throw_if_error();
        }

        const auto unpublish_start = SteadyClock::now();
        auto unpublish = node->unpublish(message_key);
        unpublish.get();
        unpublish.status().throw_if_error();
        const auto unpublish_end = SteadyClock::now();
        trace_message_kv("MESSAGE_KV_ACK_CLEANUP key=" + message_key +
                         " unsubscribe_ack_us=" + std::to_string(elapsed_us(unsubscribe_start, unsubscribe_end)) +
                         " unpublish_msg_us=" + std::to_string(elapsed_us(unpublish_start, unpublish_end)));
    }

    void recv_one_locked(const std::string& key,
                         const zerokv::MemoryRegion::Ptr& region,
                         size_t length,
                         size_t offset,
                         std::chrono::milliseconds timeout) {
        auto subscribe_future = node->subscribe(key);
        subscribe_future.status().throw_if_error();
        subscribe_future.get();

        auto unsubscribe_on_exit = [&]() {
            auto unsubscribe_future = node->unsubscribe(key);
            if (unsubscribe_future.status().ok()) {
                unsubscribe_future.get();
            }
        };

        auto try_fetch = [&]() {
            auto fetch = node->fetch_to(key, region, length, offset);
            if (!fetch.status().ok()) {
                if (fetch.status().code() == ErrorCode::kInvalidArgument) {
                    return false;
                }
                fetch.status().throw_if_error();
            }
            fetch.get();
            if (!fetch.status().ok()) {
                if (fetch.status().code() == ErrorCode::kInvalidArgument) {
                    return false;
                }
                fetch.status().throw_if_error();
            }
            return true;
        };

        if (!try_fetch()) {
            auto status = node->wait_for_subscription_event(key, timeout);
            if (!status.ok()) {
                unsubscribe_on_exit();
                status.throw_if_error();
            }
            if (!try_fetch()) {
                unsubscribe_on_exit();
                Status(ErrorCode::kTimeout, "timed out fetching key after event: " + key)
                    .throw_if_error();
            }
        }

        unsubscribe_on_exit();
        const auto ack_start = SteadyClock::now();
        publish_ack_locked(key);
        const auto ack_end = SteadyClock::now();
        record_owned_ack_keys_locked({make_ack_key(key)});
        trace_message_kv("MESSAGE_KV_RECV_ONE key=" + key +
                         " bytes=" + std::to_string(length) +
                         " ack_publish_us=" + std::to_string(elapsed_us(ack_start, ack_end)));
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

    const auto publish_start = SteadyClock::now();
    auto publish = impl_->node->publish(key, data, size);
    publish.get();
    publish.status().throw_if_error();
    const auto publish_end = SteadyClock::now();
    trace_message_kv("MESSAGE_KV_SEND_PUBLISH_DONE key=" + key +
                     " bytes=" + std::to_string(size) +
                     " publish_us=" + std::to_string(elapsed_us(publish_start, publish_end)));
    impl_->wait_for_ack_and_cleanup_message_locked(key);
    const auto send_end = SteadyClock::now();
    trace_message_kv("MESSAGE_KV_SEND key=" + key +
                     " bytes=" + std::to_string(size) +
                     " publish_us=" + std::to_string(elapsed_us(publish_start, publish_end)) +
                     " post_publish_us=" + std::to_string(elapsed_us(publish_end, send_end)) +
                     " total_us=" + std::to_string(elapsed_us(publish_start, send_end)));
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

    const auto publish_start = SteadyClock::now();
    auto publish = impl_->node->publish_region(key, region, size);
    publish.get();
    publish.status().throw_if_error();
    const auto publish_end = SteadyClock::now();
    trace_message_kv("MESSAGE_KV_SEND_REGION_PUBLISH_DONE key=" + key +
                     " bytes=" + std::to_string(size) +
                     " publish_us=" + std::to_string(elapsed_us(publish_start, publish_end)));
    impl_->wait_for_ack_and_cleanup_message_locked(key);
    const auto send_end = SteadyClock::now();
    trace_message_kv("MESSAGE_KV_SEND_REGION key=" + key +
                     " bytes=" + std::to_string(size) +
                     " publish_us=" + std::to_string(elapsed_us(publish_start, publish_end)) +
                     " post_publish_us=" + std::to_string(elapsed_us(publish_end, send_end)) +
                     " total_us=" + std::to_string(elapsed_us(publish_start, send_end)));
}

void MessageKV::recv(const std::string& key,
                     const zerokv::MemoryRegion::Ptr& region,
                     size_t length,
                     size_t offset,
                     std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->sweep_cleanup_locked();
    if (key.empty()) {
        throw std::system_error(make_error_code(ErrorCode::kInvalidArgument));
    }
    if (!impl_->node_ready_locked()) {
        throw std::system_error(make_error_code(ErrorCode::kConnectionRefused));
    }
    impl_->recv_one_locked(key, region, length, offset, timeout);
    impl_->sweep_cleanup_locked();
}

MessageKV::BatchRecvResult MessageKV::recv_batch(const std::vector<BatchRecvItem>& items,
                                                 const zerokv::MemoryRegion::Ptr& region,
                                                 std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    validate_recv_batch_layout(items, region);
    if (!impl_->node_ready_locked()) {
        throw std::system_error(make_error_code(ErrorCode::kConnectionRefused));
    }
    impl_->sweep_cleanup_locked();

    trace_message_kv("MESSAGE_KV_RECV_BATCH_START items=" + std::to_string(items.size()) +
                     " timeout_ms=" + std::to_string(timeout.count()));

    std::vector<std::string> ordered_keys;
    ordered_keys.reserve(items.size());
    std::unordered_map<std::string, std::vector<const BatchRecvItem*>> placements_by_key;
    for (const auto& item : items) {
        auto& placements = placements_by_key[item.key];
        if (placements.empty()) {
            ordered_keys.push_back(item.key);
        }
        placements.push_back(&item);
    }

    std::vector<std::string> subscribed_keys;
    subscribed_keys.reserve(ordered_keys.size());
    for (const auto& key : ordered_keys) {
        auto subscribe_future = impl_->node->subscribe(key);
        subscribe_future.status().throw_if_error();
        subscribe_future.get();
        subscribed_keys.push_back(key);
    }

    enum class PlacementState {
        kSuccess,
        kPending,
        kFailed,
    };

    auto append_placements = [](std::vector<std::string>* out,
                                const std::vector<const BatchRecvItem*>& placements) {
        for (const auto* placement : placements) {
            out->push_back(placement->key);
        }
    };

    auto try_fetch_key = [&](const std::string& key) -> PlacementState {
        const auto& placements = placements_by_key.at(key);
        trace_message_kv("MESSAGE_KV_RECV_BATCH_FETCH_BEGIN key=" + key +
                         " placements=" + std::to_string(placements.size()));
        for (const auto* placement : placements) {
            auto fetch = impl_->node->fetch_to(key, region, placement->length, placement->offset);
            if (!fetch.status().ok()) {
                if (fetch.status().code() == ErrorCode::kInvalidArgument) {
                    trace_message_kv("MESSAGE_KV_RECV_BATCH_FETCH_PENDING key=" + key);
                    return PlacementState::kPending;
                }
                trace_message_kv("MESSAGE_KV_RECV_BATCH_FETCH_FAILED key=" + key +
                                 " code=" + std::to_string(static_cast<int>(fetch.status().code())));
                return PlacementState::kFailed;
            }
            fetch.get();
            if (!fetch.status().ok()) {
                if (fetch.status().code() == ErrorCode::kInvalidArgument) {
                    trace_message_kv("MESSAGE_KV_RECV_BATCH_FETCH_PENDING key=" + key);
                    return PlacementState::kPending;
                }
                trace_message_kv("MESSAGE_KV_RECV_BATCH_FETCH_FAILED key=" + key +
                                 " code=" + std::to_string(static_cast<int>(fetch.status().code())));
                return PlacementState::kFailed;
            }
        }
        trace_message_kv("MESSAGE_KV_RECV_BATCH_FETCH_DONE key=" + key);
        return PlacementState::kSuccess;
    };

    auto ack_completed_key = [&](const std::string& key) {
        trace_message_kv("MESSAGE_KV_RECV_BATCH_ACK_BEGIN key=" + key);
        impl_->publish_ack_locked(key);
        impl_->record_owned_ack_keys_locked({make_ack_key(key)});
        trace_message_kv("MESSAGE_KV_RECV_BATCH_ACK_DONE key=" + key);
    };

    BatchRecvResult result;
    std::unordered_set<std::string> pending;
    pending.reserve(ordered_keys.size());
    std::unordered_map<std::string, uint64_t> completed_at_us;
    completed_at_us.reserve(ordered_keys.size());
    for (const auto& key : ordered_keys) {
        auto state = try_fetch_key(key);
        if (state == PlacementState::kSuccess) {
            append_placements(&result.completed, placements_by_key.at(key));
            ack_completed_key(key);
            completed_at_us.emplace(key, 0);
        } else if (state == PlacementState::kFailed) {
            append_placements(&result.failed, placements_by_key.at(key));
        } else {
            pending.insert(key);
        }
    }

    const auto recv_start = SteadyClock::now();
    const auto deadline = recv_start + timeout;
    while (!pending.empty() && SteadyClock::now() < deadline) {
        const auto now = SteadyClock::now();
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        auto event = impl_->node->wait_for_any_subscription_event(
            std::vector<std::string>(pending.begin(), pending.end()), remaining);
        if (!event.has_value()) {
            break;
        }
        if (!pending.count(event->key)) {
            continue;
        }
        trace_message_kv("MESSAGE_KV_RECV_BATCH_WAIT_ANY_MATCH key=" + event->key);
        auto state = try_fetch_key(event->key);
        if (state == PlacementState::kSuccess) {
            append_placements(&result.completed, placements_by_key.at(event->key));
            ack_completed_key(event->key);
            completed_at_us[event->key] = elapsed_us(recv_start, SteadyClock::now());
            pending.erase(event->key);
        } else if (state == PlacementState::kFailed) {
            append_placements(&result.failed, placements_by_key.at(event->key));
            pending.erase(event->key);
        }
    }
    const auto recv_end = SteadyClock::now();

    for (const auto& key : ordered_keys) {
        if (pending.count(key)) {
            result.timed_out.push_back(key);
        }
    }
    result.completed_all = (result.completed.size() == items.size());

    for (const auto& key : subscribed_keys) {
        auto unsubscribe_future = impl_->node->unsubscribe(key);
        if (unsubscribe_future.status().ok()) {
            unsubscribe_future.get();
        }
    }

    impl_->sweep_cleanup_locked();
    uint64_t first_complete_us = 0;
    uint64_t last_complete_us = 0;
    bool first_set = false;
    for (const auto& key : ordered_keys) {
        auto it = completed_at_us.find(key);
        if (it == completed_at_us.end()) {
            continue;
        }
        if (!first_set || it->second < first_complete_us) {
            first_complete_us = it->second;
            first_set = true;
        }
        if (it->second > last_complete_us) {
            last_complete_us = it->second;
        }
    }
    trace_message_kv("MESSAGE_KV_RECV_BATCH items=" + std::to_string(items.size()) +
                     " fetch_wait_us=" + std::to_string(elapsed_us(recv_start, recv_end)) +
                     " ack_publish_us=0" +
                     " first_complete_us=" + std::to_string(first_complete_us) +
                     " last_complete_us=" + std::to_string(last_complete_us) +
                     " completion_window_us=" +
                         std::to_string(first_set ? (last_complete_us - first_complete_us) : 0) +
                     " completed=" + std::to_string(result.completed.size()) +
                     " failed=" + std::to_string(result.failed.size()) +
                     " timed_out=" + std::to_string(result.timed_out.size()));
    return result;
}

}  // namespace zerokv
