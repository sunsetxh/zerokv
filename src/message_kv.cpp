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

struct PendingMessage {
    std::string message_key;
    std::string ack_key;
};

template <typename Fn>
class ScopeExit {
public:
    explicit ScopeExit(Fn fn) : fn_(std::move(fn)) {}
    ~ScopeExit() {
        if (active_) {
            fn_();
        }
    }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

    ScopeExit(ScopeExit&& other) noexcept
        : fn_(std::move(other.fn_)), active_(other.active_) {
        other.active_ = false;
    }

    ScopeExit& operator=(ScopeExit&&) = delete;

    void release() noexcept {
        active_ = false;
    }

private:
    Fn fn_;
    bool active_ = true;
};

template <typename Fn>
ScopeExit<Fn> make_scope_exit(Fn fn) {
    return ScopeExit<Fn>(std::move(fn));
}

}  // namespace

struct MessageKV::Impl {
    explicit Impl(const zerokv::Config& config) : cfg(config) {}

    zerokv::Config cfg;
    zerokv::kv::KVNode::Ptr node;
    std::mutex mu;
    bool running = false;
    std::vector<PendingMessage> pending_messages;
    std::vector<std::string> owned_ack_keys;

    bool node_ready_locked() const noexcept {
        return running && static_cast<bool>(node);
    }

    void sweep_sender_cleanup_locked() {
        if (!node_ready_locked() || pending_messages.empty()) {
            return;
        }

        std::vector<PendingMessage> remaining;
        remaining.reserve(pending_messages.size());
        for (const auto& pending : pending_messages) {
            auto ack_status = node->wait_for_key(pending.ack_key, std::chrono::milliseconds{0});
            if (!ack_status.ok()) {
                remaining.push_back(pending);
                continue;
            }

            auto unpublish = node->unpublish(pending.message_key);
            unpublish.get();
            if (!unpublish.status().ok()) {
                remaining.push_back(pending);
            }
        }

        pending_messages.swap(remaining);
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
        sweep_sender_cleanup_locked();
        sweep_receiver_ack_cleanup_locked();
    }

    void record_owned_ack_keys_locked(std::vector<std::string> ack_keys) {
        owned_ack_keys.reserve(owned_ack_keys.size() + ack_keys.size());
        for (auto& ack_key : ack_keys) {
            owned_ack_keys.push_back(std::move(ack_key));
        }
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
    impl_->pending_messages.clear();
    impl_->owned_ack_keys.clear();
}

void MessageKV::send(const std::string& key, const void* data, size_t size) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto guard = make_scope_exit([this] { impl_->sweep_cleanup_locked(); });
    (void)guard;
    impl_->sweep_cleanup_locked();
    if (!impl_->node_ready_locked()) {
        throw std::system_error(make_error_code(ErrorCode::kConnectionRefused));
    }
    if (key.empty()) {
        throw std::system_error(make_error_code(ErrorCode::kInvalidArgument));
    }
    if (size > 0 && data == nullptr) {
        throw std::system_error(make_error_code(ErrorCode::kInvalidArgument));
    }

    auto publish = impl_->node->publish(key, data, size);
    publish.get();
    publish.status().throw_if_error();
    impl_->pending_messages.push_back(PendingMessage{
        .message_key = key,
        .ack_key = make_ack_key(key),
    });
}

void MessageKV::send_region(const std::string& key,
                            const zerokv::MemoryRegion::Ptr& region,
                            size_t size) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto guard = make_scope_exit([this] { impl_->sweep_cleanup_locked(); });
    (void)guard;
    impl_->sweep_cleanup_locked();
    if (!impl_->node_ready_locked()) {
        throw std::system_error(make_error_code(ErrorCode::kConnectionRefused));
    }
    if (key.empty()) {
        throw std::system_error(make_error_code(ErrorCode::kInvalidArgument));
    }
    if (!region) {
        throw std::system_error(make_error_code(ErrorCode::kInvalidArgument));
    }
    if (size > region->length()) {
        throw std::system_error(make_error_code(ErrorCode::kInvalidArgument));
    }

    auto publish = impl_->node->publish_region(key, region, size);
    publish.get();
    publish.status().throw_if_error();
    impl_->pending_messages.push_back(PendingMessage{
        .message_key = key,
        .ack_key = make_ack_key(key),
    });
}

void MessageKV::recv(const std::string& key,
                     const zerokv::MemoryRegion::Ptr& region,
                     size_t length,
                     size_t offset,
                     std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto guard = make_scope_exit([this] { impl_->sweep_cleanup_locked(); });
    (void)guard;
    impl_->sweep_cleanup_locked();
    if (!impl_->node_ready_locked()) {
        throw std::system_error(make_error_code(ErrorCode::kConnectionRefused));
    }

    auto status = impl_->node->wait_for_key(key, timeout);
    status.throw_if_error();

    auto fetch = impl_->node->fetch_to(key, region, length, offset);
    fetch.get();
    fetch.status().throw_if_error();

    std::vector<std::string> new_ack_keys;
    new_ack_keys.push_back(make_ack_key(key));
    auto ack = impl_->node->publish(new_ack_keys.back(), &kAckMarker, sizeof(kAckMarker));
    ack.get();
    ack.status().throw_if_error();

    impl_->sweep_cleanup_locked();
    guard.release();
    impl_->record_owned_ack_keys_locked(std::move(new_ack_keys));
}

MessageKV::BatchRecvResult MessageKV::recv_batch(const std::vector<BatchRecvItem>& items,
                                                 const zerokv::MemoryRegion::Ptr& region,
                                                 std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto guard = make_scope_exit([this] { impl_->sweep_cleanup_locked(); });
    (void)guard;
    impl_->sweep_cleanup_locked();
    if (!impl_->node_ready_locked()) {
        throw std::system_error(make_error_code(ErrorCode::kConnectionRefused));
    }

    BatchRecvResult result;
    std::vector<std::string> new_ack_keys;
    new_ack_keys.reserve(items.size());
    for (const auto& item : items) {
        auto status = impl_->node->wait_for_key(item.key, timeout);
        status.throw_if_error();

        auto fetch = impl_->node->fetch_to(item.key, region, item.length, item.offset);
        fetch.get();
        fetch.status().throw_if_error();

        result.completed.push_back(item.key);
        new_ack_keys.push_back(make_ack_key(item.key));

        auto ack = impl_->node->publish(new_ack_keys.back(), &kAckMarker, sizeof(kAckMarker));
        ack.get();
        ack.status().throw_if_error();
    }
    result.completed_all = true;
    impl_->sweep_cleanup_locked();
    guard.release();
    impl_->record_owned_ack_keys_locked(std::move(new_ack_keys));
    return result;
}

}  // namespace zerokv
