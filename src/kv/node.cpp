#include "axon/kv.h"

#include "axon/endpoint.h"
#include "axon/worker.h"

#include "kv/protocol.h"
#include "kv/tcp_framing.h"
#include "kv/tcp_transport.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace axon::kv {

namespace {

using SteadyClock = std::chrono::steady_clock;

uint64_t elapsed_us(SteadyClock::time_point start, SteadyClock::time_point end) {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    if (end > start && us == 0) {
        return 1;
    }
    return static_cast<uint64_t>(us);
}

std::string generate_node_id() {
    static std::atomic<uint64_t> next_id{1};
    const auto value = next_id.fetch_add(1);
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return "node-" + std::to_string(static_cast<unsigned long long>(now)) + "-" +
           std::to_string(static_cast<unsigned long long>(value));
}

Status status_from_msg(detail::MsgStatus status, const std::string& message, ErrorCode fallback) {
    switch (status) {
        case detail::MsgStatus::kOk:
            return Status::OK();
        case detail::MsgStatus::kNotFound:
            return Status(ErrorCode::kInvalidArgument, message.empty() ? "not found" : message);
        case detail::MsgStatus::kAlreadyExists:
        case detail::MsgStatus::kInvalidRequest:
            return Status(ErrorCode::kInvalidArgument, message.empty() ? "invalid request" : message);
        case detail::MsgStatus::kStaleOwner:
            return Status(ErrorCode::kConnectionReset, message.empty() ? "stale owner" : message);
        case detail::MsgStatus::kInternalError:
        default:
            return Status(fallback, message.empty() ? "internal error" : message);
    }
}

}  // namespace

struct KVNode::Impl {
    explicit Impl(const axon::Config& cfg) : cfg_(cfg) {}

    struct PublishedObject {
        MemoryRegion::Ptr region;
        RemoteKey rkey;
        size_t size = 0;
        uint64_t version = 0;
    };

    Config cfg_;
    Context::Ptr context_;
    Worker::Ptr worker_;
    Listener::Ptr listener_;
    std::atomic<bool> running_{false};
    int control_fd_ = -1;
    std::string server_addr_;
    std::string local_data_addr_;
    std::string node_id_;
    std::mutex control_mu_;  // Serializes control-plane request/response operations.
    std::mutex published_mu_;
    std::mutex peer_mu_;
    std::mutex metrics_mu_;
    std::unordered_map<std::string, PublishedObject> published_;
    std::unordered_map<std::string, Endpoint::Ptr> peer_eps_;
    std::vector<Endpoint::Ptr> inbound_eps_;
    std::atomic<uint64_t> next_request_id_{1};
    std::optional<PublishMetrics> last_publish_metrics_;
    std::optional<FetchMetrics> last_fetch_metrics_;

    void record_publish_metrics(PublishMetrics metrics) {
        std::lock_guard<std::mutex> lock(metrics_mu_);
        last_publish_metrics_ = metrics;
    }

    void record_fetch_metrics(FetchMetrics metrics) {
        std::lock_guard<std::mutex> lock(metrics_mu_);
        last_fetch_metrics_ = metrics;
    }

    Status register_with_server() {
        detail::RegisterNodeRequest req;
        req.node_id = node_id_;
        req.control_addr.clear();
        req.data_addr = local_data_addr_;

        auto payload = detail::encode(req);
        const uint64_t request_id = next_request_id_.fetch_add(1);
        if (!detail::send_frame(control_fd_, detail::MsgType::kRegisterNode, request_id, payload)) {
            return Status(ErrorCode::kConnectionReset, "failed to send register request");
        }

        detail::MsgHeader header;
        std::vector<uint8_t> response_payload;
        if (!detail::recv_frame(control_fd_, &header, &response_payload)) {
            return Status(ErrorCode::kConnectionReset, "failed to read register response");
        }
        if (header.request_id != request_id) {
            return Status(ErrorCode::kInternalError, "register response request_id mismatch");
        }

        const auto type = static_cast<detail::MsgType>(header.type);
        if (type == detail::MsgType::kError) {
            auto err = detail::decode_error_response(response_payload);
            if (!err.has_value()) {
                return Status(ErrorCode::kInternalError, "failed to decode register error response");
            }
            return status_from_msg(err->status, err->message, ErrorCode::kInternalError);
        }
        if (type != detail::MsgType::kRegisterNodeResp) {
            return Status(ErrorCode::kInternalError, "unexpected register response type");
        }

        auto resp = detail::decode_register_node_response(response_payload);
        if (!resp.has_value()) {
            return Status(ErrorCode::kInternalError, "failed to decode register response");
        }

        auto status = status_from_msg(resp->status, resp->message, ErrorCode::kInternalError);
        if (!status.ok()) {
            return status;
        }
        if (resp->assigned_node_id.empty()) {
            return Status(ErrorCode::kInternalError, "server returned empty node_id");
        }
        node_id_ = resp->assigned_node_id;
        return Status::OK();
    }

    Status put_meta(const detail::KeyMetadata& meta) {
        std::lock_guard<std::mutex> lock(control_mu_);
        if (control_fd_ < 0) {
            return Status(ErrorCode::kConnectionReset, "control connection is closed");
        }

        detail::PutMetaRequest req;
        req.metadata = meta;

        auto payload = detail::encode(req);
        const uint64_t request_id = next_request_id_.fetch_add(1);
        if (!detail::send_frame(control_fd_, detail::MsgType::kPutMeta, request_id, payload)) {
            return Status(ErrorCode::kConnectionReset, "failed to send put_meta request");
        }

        detail::MsgHeader header;
        std::vector<uint8_t> response_payload;
        if (!detail::recv_frame(control_fd_, &header, &response_payload)) {
            return Status(ErrorCode::kConnectionReset, "failed to read put_meta response");
        }
        if (header.request_id != request_id) {
            return Status(ErrorCode::kInternalError, "put_meta response request_id mismatch");
        }

        const auto type = static_cast<detail::MsgType>(header.type);
        if (type == detail::MsgType::kError) {
            auto err = detail::decode_error_response(response_payload);
            if (!err.has_value()) {
                return Status(ErrorCode::kInternalError, "failed to decode put_meta error response");
            }
            return status_from_msg(err->status, err->message, ErrorCode::kInternalError);
        }
        if (type != detail::MsgType::kPutMetaResp) {
            return Status(ErrorCode::kInternalError, "unexpected put_meta response type");
        }

        auto resp = detail::decode_put_meta_response(response_payload);
        if (!resp.has_value()) {
            return Status(ErrorCode::kInternalError, "failed to decode put_meta response");
        }
        return status_from_msg(resp->status, resp->message, ErrorCode::kInternalError);
    }

    Status unpublish_remote(const std::string& key) {
        std::lock_guard<std::mutex> lock(control_mu_);
        if (control_fd_ < 0) {
            return Status(ErrorCode::kConnectionReset, "control connection is closed");
        }

        detail::UnpublishRequest req;
        req.key = key;
        req.owner_node_id = node_id_;

        auto payload = detail::encode(req);
        const uint64_t request_id = next_request_id_.fetch_add(1);
        if (!detail::send_frame(control_fd_, detail::MsgType::kUnpublish, request_id, payload)) {
            return Status(ErrorCode::kConnectionReset, "failed to send unpublish request");
        }

        detail::MsgHeader header;
        std::vector<uint8_t> response_payload;
        if (!detail::recv_frame(control_fd_, &header, &response_payload)) {
            return Status(ErrorCode::kConnectionReset, "failed to read unpublish response");
        }
        if (header.request_id != request_id) {
            return Status(ErrorCode::kInternalError, "unpublish response request_id mismatch");
        }

        const auto type = static_cast<detail::MsgType>(header.type);
        if (type == detail::MsgType::kError) {
            auto err = detail::decode_error_response(response_payload);
            if (!err.has_value()) {
                return Status(ErrorCode::kInternalError, "failed to decode unpublish error response");
            }
            return status_from_msg(err->status, err->message, ErrorCode::kInternalError);
        }
        if (type != detail::MsgType::kUnpublishResp) {
            return Status(ErrorCode::kInternalError, "unexpected unpublish response type");
        }

        auto resp = detail::decode_unpublish_response(response_payload);
        if (!resp.has_value()) {
            return Status(ErrorCode::kInternalError, "failed to decode unpublish response");
        }
        return status_from_msg(resp->status, resp->message, ErrorCode::kInternalError);
    }

    std::optional<detail::KeyMetadata> get_meta(const std::string& key,
                                                Status* status_out,
                                                FetchMetrics* metrics = nullptr) {
        std::lock_guard<std::mutex> lock(control_mu_);
        if (control_fd_ < 0) {
            if (status_out) {
                *status_out = Status(ErrorCode::kConnectionReset, "control connection is closed");
            }
            return std::nullopt;
        }

        detail::GetMetaRequest req;
        req.key = key;
        auto payload = detail::encode(req);
        const uint64_t request_id = next_request_id_.fetch_add(1);
        const auto rpc_start = SteadyClock::now();
        if (!detail::send_frame(control_fd_, detail::MsgType::kGetMeta, request_id, payload)) {
            if (metrics) {
                metrics->get_meta_rpc_us = elapsed_us(rpc_start, SteadyClock::now());
            }
            if (status_out) {
                *status_out = Status(ErrorCode::kConnectionReset, "failed to send get_meta request");
            }
            return std::nullopt;
        }

        detail::MsgHeader header;
        std::vector<uint8_t> response_payload;
        if (!detail::recv_frame(control_fd_, &header, &response_payload)) {
            if (metrics) {
                metrics->get_meta_rpc_us = elapsed_us(rpc_start, SteadyClock::now());
            }
            if (status_out) {
                *status_out = Status(ErrorCode::kConnectionReset, "failed to read get_meta response");
            }
            return std::nullopt;
        }
        if (metrics) {
            metrics->get_meta_rpc_us = elapsed_us(rpc_start, SteadyClock::now());
        }
        if (header.request_id != request_id) {
            if (status_out) {
                *status_out = Status(ErrorCode::kInternalError, "get_meta response request_id mismatch");
            }
            return std::nullopt;
        }

        const auto type = static_cast<detail::MsgType>(header.type);
        if (type == detail::MsgType::kError) {
            auto err = detail::decode_error_response(response_payload);
            if (!err.has_value()) {
                if (status_out) {
                    *status_out = Status(ErrorCode::kInternalError, "failed to decode get_meta error response");
                }
                return std::nullopt;
            }
            if (status_out) {
                *status_out = status_from_msg(err->status, err->message, ErrorCode::kInternalError);
            }
            return std::nullopt;
        }
        if (type != detail::MsgType::kGetMetaResp) {
            if (status_out) {
                *status_out = Status(ErrorCode::kInternalError, "unexpected get_meta response type");
            }
            return std::nullopt;
        }

        auto resp = detail::decode_get_meta_response(response_payload);
        if (!resp.has_value()) {
            if (status_out) {
                *status_out = Status(ErrorCode::kInternalError, "failed to decode get_meta response");
            }
            return std::nullopt;
        }

        auto status = status_from_msg(resp->status, resp->message, ErrorCode::kInternalError);
        if (!status.ok()) {
            if (status_out) {
                *status_out = status;
            }
            return std::nullopt;
        }
        if (!resp->metadata.has_value()) {
            if (status_out) {
                *status_out = Status(ErrorCode::kInternalError, "missing metadata in get_meta response");
            }
            return std::nullopt;
        }

        if (status_out) {
            *status_out = Status::OK();
        }
        return resp->metadata;
    }

    Endpoint::Ptr get_or_connect_peer(const std::string& node_id,
                                      const std::string& data_addr,
                                      Status* status_out) {
        {
            std::lock_guard<std::mutex> lock(peer_mu_);
            auto it = peer_eps_.find(node_id);
            if (it != peer_eps_.end() && it->second) {
                if (status_out) {
                    *status_out = Status::OK();
                }
                return it->second;
            }
        }

        if (!worker_) {
            if (status_out) {
                *status_out = Status(ErrorCode::kInternalError, "worker is not initialized");
            }
            return nullptr;
        }

        auto connect = worker_->connect(data_addr);
        auto ep = connect.get();
        if (!ep) {
            if (status_out) {
                auto status = connect.status();
                *status_out = status.ok() ? Status(ErrorCode::kConnectionRefused, "failed to connect to peer")
                                          : status;
            }
            return nullptr;
        }

        {
            std::lock_guard<std::mutex> lock(peer_mu_);
            auto [it, inserted] = peer_eps_.emplace(node_id, ep);
            if (!inserted && it->second) {
                ep = it->second;
            } else {
                it->second = ep;
            }
        }

        if (status_out) {
            *status_out = Status::OK();
        }
        return ep;
    }

    Future<void> fetch_to_impl(const detail::KeyMetadata& meta,
                               const MemoryRegion::Ptr& local_region,
                               size_t length,
                               size_t local_offset,
                               FetchMetrics* metrics) {
        if (!running_.load()) {
            return Future<void>::make_error(Status(ErrorCode::kConnectionReset, "KVNode is not running"));
        }
        if (!local_region) {
            return Future<void>::make_error(Status(ErrorCode::kInvalidArgument, "local region is required"));
        }
        if (local_offset > local_region->length()) {
            return Future<void>::make_error(Status(ErrorCode::kInvalidArgument, "local offset is out of range"));
        }
        if (meta.size > length || local_offset + meta.size > local_region->length()) {
            return Future<void>::make_error(Status(ErrorCode::kInvalidArgument, "local region is too small"));
        }

        Status peer_status;
        const auto connect_start = SteadyClock::now();
        auto peer = get_or_connect_peer(meta.owner_node_id, meta.owner_data_addr, &peer_status);
        if (metrics) {
            metrics->peer_connect_us = elapsed_us(connect_start, SteadyClock::now());
        }
        if (!peer) {
            return Future<void>::make_error(peer_status);
        }

        const auto prepare_start = SteadyClock::now();
        RemoteKey rkey;
        rkey.data = meta.rkey;
        auto get = peer->get(local_region, local_offset, meta.remote_addr, rkey, meta.size);
        const auto prepare_end = SteadyClock::now();
        if (metrics) {
            metrics->rdma_prepare_us = elapsed_us(prepare_start, prepare_end);
        }
        const auto get_start = SteadyClock::now();
        get.get();
        if (metrics) {
            metrics->rdma_get_us = elapsed_us(get_start, SteadyClock::now());
        }
        auto status = get.status();
        if (!status.ok()) {
            return Future<void>::make_error(status);
        }
        return Future<void>::make_ready();
    }

    Future<void> publish_impl(const std::string& key,
                              const MemoryRegion::Ptr& region,
                              size_t size,
                              PublishMetrics* metrics) {
        if (!running_.load()) {
            return Future<void>::make_error(Status(ErrorCode::kConnectionReset, "KVNode is not running"));
        }
        if (key.empty()) {
            return Future<void>::make_error(Status(ErrorCode::kInvalidArgument, "key is required"));
        }
        if (!region || size == 0 || size > region->length()) {
            return Future<void>::make_error(Status(ErrorCode::kInvalidArgument, "invalid region or size"));
        }

        PublishedObject object;
        object.region = region;
        const auto pack_start = SteadyClock::now();
        object.rkey = region->remote_key();
        if (metrics) {
            metrics->pack_rkey_us = elapsed_us(pack_start, SteadyClock::now());
        }
        object.size = size;
        {
            std::lock_guard<std::mutex> lock(published_mu_);
            auto existing = published_.find(key);
            object.version = (existing == published_.end()) ? 1 : (existing->second.version + 1);
        }

        detail::KeyMetadata meta;
        meta.key = key;
        meta.owner_node_id = node_id_;
        meta.owner_data_addr = local_data_addr_;
        meta.remote_addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(region->address()));
        meta.rkey = object.rkey.data;
        meta.size = size;
        meta.version = object.version;

        const auto rpc_start = SteadyClock::now();
        auto status = put_meta(meta);
        if (metrics) {
            metrics->put_meta_rpc_us = elapsed_us(rpc_start, SteadyClock::now());
        }
        if (!status.ok()) {
            return Future<void>::make_error(status);
        }

        {
            std::lock_guard<std::mutex> lock(published_mu_);
            published_[key] = std::move(object);
        }
        return Future<void>::make_ready();
    }
};

KVNode::KVNode(const axon::Config& cfg) : impl_(std::make_unique<Impl>(cfg)) {}

KVNode::~KVNode() = default;

KVNode::Ptr KVNode::create(const axon::Config& cfg) {
    return Ptr(new KVNode(cfg));
}

Status KVNode::start(const NodeConfig& cfg) {
    if (!impl_) {
        return Status(ErrorCode::kInternalError, "KVNode not initialized");
    }
    if (impl_->running_.load()) {
        return Status::OK();
    }
    if (cfg.server_addr.empty()) {
        return Status(ErrorCode::kInvalidArgument, "server_addr is required");
    }
    if (cfg.local_data_addr.empty()) {
        return Status(ErrorCode::kInvalidArgument, "local_data_addr is required");
    }

    impl_->server_addr_ = cfg.server_addr;
    impl_->local_data_addr_ = cfg.local_data_addr;
    impl_->node_id_ = cfg.node_id.empty() ? generate_node_id() : cfg.node_id;
    impl_->context_ = Context::create(impl_->cfg_);
    if (!impl_->context_) {
        impl_->node_id_.clear();
        return Status(ErrorCode::kTransportError, "failed to create axon context");
    }
    impl_->worker_ = Worker::create(impl_->context_);
    if (!impl_->worker_) {
        impl_->context_.reset();
        impl_->node_id_.clear();
        return Status(ErrorCode::kTransportError, "failed to create axon worker");
    }
    impl_->listener_ = impl_->worker_->listen(cfg.local_data_addr, [impl = impl_.get()](Endpoint::Ptr ep) {
        std::lock_guard<std::mutex> lock(impl->peer_mu_);
        impl->inbound_eps_.push_back(std::move(ep));
    });
    if (!impl_->listener_) {
        impl_->worker_.reset();
        impl_->context_.reset();
        impl_->node_id_.clear();
        return Status(ErrorCode::kTransportError, "failed to create KVNode data listener");
    }
    impl_->local_data_addr_ = impl_->listener_->address();
    impl_->worker_->start_progress_thread();

    std::string error;
    impl_->control_fd_ = detail::TcpTransport::connect(impl_->server_addr_, &error);
    if (impl_->control_fd_ < 0) {
        impl_->listener_->close();
        impl_->listener_.reset();
        impl_->worker_->stop_progress_thread();
        impl_->worker_.reset();
        impl_->context_.reset();
        impl_->node_id_.clear();
        return Status(ErrorCode::kConnectionRefused, error.empty() ? "failed to connect to KV server" : error);
    }

    auto status = impl_->register_with_server();
    if (!status.ok()) {
        detail::TcpTransport::close_fd(&impl_->control_fd_);
        impl_->listener_->close();
        impl_->listener_.reset();
        impl_->worker_->stop_progress_thread();
        impl_->worker_.reset();
        impl_->context_.reset();
        impl_->node_id_.clear();
        return status;
    }

    impl_->running_.store(true);
    return Status::OK();
}

void KVNode::stop() {
    if (!impl_ || !impl_->running_.exchange(false)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->control_mu_);
        detail::TcpTransport::close_fd(&impl_->control_fd_);
    }
    {
        std::lock_guard<std::mutex> lock(impl_->published_mu_);
        impl_->published_.clear();
    }
    if (impl_->listener_) {
        impl_->listener_->close();
        impl_->listener_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(impl_->peer_mu_);
        impl_->peer_eps_.clear();
        impl_->inbound_eps_.clear();
    }
    if (impl_->worker_) {
        impl_->worker_->stop_progress_thread();
        impl_->worker_.reset();
    }
    impl_->context_.reset();
}

bool KVNode::is_running() const noexcept {
    return impl_ && impl_->running_.load();
}

std::string KVNode::node_id() const {
    return impl_ ? impl_->node_id_ : std::string{};
}

size_t KVNode::published_count() const noexcept {
    if (!impl_) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(impl_->published_mu_);
    return impl_->published_.size();
}

std::optional<PublishMetrics> KVNode::last_publish_metrics() const {
    if (!impl_) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(impl_->metrics_mu_);
    return impl_->last_publish_metrics_;
}

std::optional<FetchMetrics> KVNode::last_fetch_metrics() const {
    if (!impl_) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(impl_->metrics_mu_);
    return impl_->last_fetch_metrics_;
}

Future<void> KVNode::publish(const std::string& key, const void* data, size_t size) {
    if (!impl_) {
        return Future<void>::make_error(Status(ErrorCode::kInternalError, "KVNode not initialized"));
    }
    PublishMetrics metrics;
    const auto total_start = SteadyClock::now();
    if (data == nullptr || size == 0) {
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_publish_metrics(metrics);
        return Future<void>::make_error(Status(ErrorCode::kInvalidArgument, "data and size are required"));
    }
    const auto prepare_start = SteadyClock::now();
    auto region = MemoryRegion::allocate(impl_->context_, size);
    if (!region) {
        metrics.prepare_region_us = elapsed_us(prepare_start, SteadyClock::now());
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_publish_metrics(metrics);
        return Future<void>::make_error(Status(ErrorCode::kRegistrationFailed, "failed to allocate publish region"));
    }
    std::memcpy(region->address(), data, size);
    metrics.prepare_region_us = elapsed_us(prepare_start, SteadyClock::now());
    auto publish = impl_->publish_impl(key, region, size, &metrics);
    metrics.total_us = elapsed_us(total_start, SteadyClock::now());
    metrics.ok = publish.status().ok();
    impl_->record_publish_metrics(metrics);
    return publish;
}

Future<void> KVNode::publish_region(const std::string& key,
                                    const axon::MemoryRegion::Ptr& region,
                                    size_t size) {
    if (!impl_) {
        return Future<void>::make_error(Status(ErrorCode::kInternalError, "KVNode not initialized"));
    }
    PublishMetrics metrics;
    const auto total_start = SteadyClock::now();
    const auto prepare_start = SteadyClock::now();
    metrics.prepare_region_us = elapsed_us(prepare_start, SteadyClock::now());
    auto publish = impl_->publish_impl(key, region, size, &metrics);
    metrics.total_us = elapsed_us(total_start, SteadyClock::now());
    metrics.ok = publish.status().ok();
    impl_->record_publish_metrics(metrics);
    return publish;
}

Future<FetchResult> KVNode::fetch(const std::string& key) {
    if (!impl_) {
        return Future<FetchResult>::make_error(Status(ErrorCode::kInternalError, "KVNode not initialized"));
    }

    Status status;
    FetchMetrics metrics;
    const auto total_start = SteadyClock::now();
    auto meta = impl_->get_meta(key, &status, &metrics);
    if (!meta.has_value()) {
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_fetch_metrics(metrics);
        return Future<FetchResult>::make_error(status);
    }

    const auto prepare_start = SteadyClock::now();
    auto local_region = MemoryRegion::allocate(impl_->context_, meta->size);
    if (!local_region) {
        metrics.local_buffer_prepare_us = elapsed_us(prepare_start, SteadyClock::now());
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_fetch_metrics(metrics);
        return Future<FetchResult>::make_error(
            Status(ErrorCode::kRegistrationFailed, "failed to allocate fetch buffer"));
    }
    metrics.local_buffer_prepare_us = elapsed_us(prepare_start, SteadyClock::now());

    auto fetch = impl_->fetch_to_impl(*meta, local_region, meta->size, 0, &metrics);
    if (!fetch.status().ok()) {
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_fetch_metrics(metrics);
        return Future<FetchResult>::make_error(fetch.status());
    }
    fetch.get();
    if (!fetch.status().ok()) {
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_fetch_metrics(metrics);
        return Future<FetchResult>::make_error(fetch.status());
    }

    FetchResult result;
    result.owner_node_id = meta->owner_node_id;
    result.version = meta->version;
    result.data.resize(meta->size);
    std::memcpy(result.data.data(), local_region->address(), meta->size);
    metrics.total_us = elapsed_us(total_start, SteadyClock::now());
    metrics.ok = true;
    impl_->record_fetch_metrics(metrics);
    return Future<FetchResult>::make_ready(std::move(result));
}

Future<void> KVNode::fetch_to(const std::string& key,
                              const axon::MemoryRegion::Ptr& local_region,
                              size_t length,
                              size_t local_offset) {
    if (!impl_) {
        return Future<void>::make_error(Status(ErrorCode::kInternalError, "KVNode not initialized"));
    }

    FetchMetrics metrics;
    const auto total_start = SteadyClock::now();
    const auto prepare_start = SteadyClock::now();
    metrics.local_buffer_prepare_us = elapsed_us(prepare_start, SteadyClock::now());

    Status status;
    auto meta = impl_->get_meta(key, &status, &metrics);
    if (!meta.has_value()) {
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_fetch_metrics(metrics);
        return Future<void>::make_error(status);
    }
    auto fetch = impl_->fetch_to_impl(*meta, local_region, length, local_offset, &metrics);
    metrics.total_us = elapsed_us(total_start, SteadyClock::now());
    metrics.ok = fetch.status().ok();
    impl_->record_fetch_metrics(metrics);
    return fetch;
}

Future<void> KVNode::unpublish(const std::string& key) {
    if (!impl_) {
        return Future<void>::make_error(Status(ErrorCode::kInternalError, "KVNode not initialized"));
    }
    if (key.empty()) {
        return Future<void>::make_error(Status(ErrorCode::kInvalidArgument, "key is required"));
    }

    {
        std::lock_guard<std::mutex> lock(impl_->published_mu_);
        if (impl_->published_.find(key) == impl_->published_.end()) {
            return Future<void>::make_error(Status(ErrorCode::kInvalidArgument, "key is not published locally"));
        }
    }

    auto status = impl_->unpublish_remote(key);
    if (!status.ok()) {
        return Future<void>::make_error(status);
    }

    {
        std::lock_guard<std::mutex> lock(impl_->published_mu_);
        impl_->published_.erase(key);
    }
    return Future<void>::make_ready();
}

}  // namespace axon::kv
