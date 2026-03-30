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
constexpr uint32_t kPushInboxMagic = 0x50555831;  // "PUX1"
constexpr size_t kDefaultPushInboxCapacity = 64 * 1024;

struct PushInboxHeader {
    uint32_t magic = kPushInboxMagic;
    uint32_t key_size = 0;
    uint64_t value_size = 0;
};

uint64_t elapsed_us(SteadyClock::time_point start, SteadyClock::time_point end) {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    if (end > start && us == 0) {
        return 1;
    }
    return static_cast<uint64_t>(us);
}

std::string make_ephemeral_bind_addr(const std::string& addr) {
    const auto colon = addr.rfind(':');
    if (colon == std::string::npos || colon == 0) {
        return "0.0.0.0:0";
    }
    return addr.substr(0, colon) + ":0";
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
    int push_listen_fd_ = -1;
    std::string push_control_addr_;
    std::thread push_accept_thread_;
    int subscription_listen_fd_ = -1;
    std::string subscription_control_addr_;
    std::thread subscription_accept_thread_;
    MemoryRegion::Ptr push_inbox_region_;
    RemoteKey push_inbox_rkey_;
    size_t push_inbox_capacity_ = kDefaultPushInboxCapacity;
    std::mutex push_mu_;
    bool push_busy_ = false;
    std::atomic<bool> running_{false};
    int control_fd_ = -1;
    std::string server_addr_;
    std::string local_data_addr_;
    std::string node_id_;
    std::mutex control_mu_;  // Serializes control-plane request/response operations.
    std::mutex published_mu_;
    std::mutex peer_mu_;
    std::mutex subscription_mu_;
    std::mutex metrics_mu_;
    std::unordered_map<std::string, PublishedObject> published_;
    std::unordered_map<std::string, Endpoint::Ptr> peer_eps_;
    std::vector<Endpoint::Ptr> inbound_eps_;
    std::vector<SubscriptionEvent> subscription_events_;
    std::atomic<uint64_t> next_request_id_{1};
    std::optional<PublishMetrics> last_publish_metrics_;
    std::optional<FetchMetrics> last_fetch_metrics_;
    std::optional<PushMetrics> last_push_metrics_;

    void record_publish_metrics(PublishMetrics metrics) {
        std::lock_guard<std::mutex> lock(metrics_mu_);
        last_publish_metrics_ = metrics;
    }

    void record_fetch_metrics(FetchMetrics metrics) {
        std::lock_guard<std::mutex> lock(metrics_mu_);
        last_fetch_metrics_ = metrics;
    }

    void record_push_metrics(PushMetrics metrics) {
        std::lock_guard<std::mutex> lock(metrics_mu_);
        last_push_metrics_ = metrics;
    }

    Status register_with_server() {
        detail::RegisterNodeRequest req;
        req.node_id = node_id_;
        req.control_addr.clear();
        req.data_addr = local_data_addr_;
        req.push_control_addr = push_control_addr_;
        req.subscription_control_addr = subscription_control_addr_;
        req.push_inbox_remote_addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(push_inbox_region_->address()));
        req.push_inbox_rkey = push_inbox_rkey_.data;
        req.push_inbox_capacity = static_cast<uint64_t>(push_inbox_capacity_);

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

    std::optional<detail::GetPushTargetResponse> get_push_target(const std::string& target_node_id,
                                                                 Status* status_out) {
        std::lock_guard<std::mutex> lock(control_mu_);
        if (control_fd_ < 0) {
            if (status_out) {
                *status_out = Status(ErrorCode::kConnectionReset, "control connection is closed");
            }
            return std::nullopt;
        }

        detail::GetPushTargetRequest req;
        req.target_node_id = target_node_id;
        auto payload = detail::encode(req);
        const uint64_t request_id = next_request_id_.fetch_add(1);
        if (!detail::send_frame(control_fd_, detail::MsgType::kGetPushTarget, request_id, payload)) {
            if (status_out) {
                *status_out = Status(ErrorCode::kConnectionReset, "failed to send get_push_target request");
            }
            return std::nullopt;
        }

        detail::MsgHeader header;
        std::vector<uint8_t> response_payload;
        if (!detail::recv_frame(control_fd_, &header, &response_payload)) {
            if (status_out) {
                *status_out = Status(ErrorCode::kConnectionReset, "failed to read get_push_target response");
            }
            return std::nullopt;
        }
        if (header.request_id != request_id) {
            if (status_out) {
                *status_out = Status(ErrorCode::kInternalError, "get_push_target response request_id mismatch");
            }
            return std::nullopt;
        }

        const auto type = static_cast<detail::MsgType>(header.type);
        if (type == detail::MsgType::kError) {
            auto err = detail::decode_error_response(response_payload);
            if (!err.has_value()) {
                if (status_out) {
                    *status_out = Status(ErrorCode::kInternalError, "failed to decode get_push_target error response");
                }
                return std::nullopt;
            }
            if (status_out) {
                *status_out = status_from_msg(err->status, err->message, ErrorCode::kInternalError);
            }
            return std::nullopt;
        }
        if (type != detail::MsgType::kGetPushTargetResp) {
            if (status_out) {
                *status_out = Status(ErrorCode::kInternalError, "unexpected get_push_target response type");
            }
            return std::nullopt;
        }

        auto resp = detail::decode_get_push_target_response(response_payload);
        if (!resp.has_value()) {
            if (status_out) {
                *status_out = Status(ErrorCode::kInternalError, "failed to decode get_push_target response");
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
        if (status_out) {
            *status_out = Status::OK();
        }
        return resp;
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

    Status subscribe_remote(const std::string& key) {
        std::lock_guard<std::mutex> lock(control_mu_);
        if (control_fd_ < 0) {
            return Status(ErrorCode::kConnectionReset, "control connection is closed");
        }

        detail::SubscribeRequest req;
        req.subscriber_node_id = node_id_;
        req.key = key;

        const uint64_t request_id = next_request_id_.fetch_add(1);
        if (!detail::send_frame(control_fd_, detail::MsgType::kSubscribe, request_id, detail::encode(req))) {
            return Status(ErrorCode::kConnectionReset, "failed to send subscribe request");
        }

        detail::MsgHeader header;
        std::vector<uint8_t> response_payload;
        if (!detail::recv_frame(control_fd_, &header, &response_payload)) {
            return Status(ErrorCode::kConnectionReset, "failed to read subscribe response");
        }
        if (header.request_id != request_id) {
            return Status(ErrorCode::kInternalError, "subscribe response request_id mismatch");
        }

        const auto type = static_cast<detail::MsgType>(header.type);
        if (type == detail::MsgType::kError) {
            auto err = detail::decode_error_response(response_payload);
            if (!err.has_value()) {
                return Status(ErrorCode::kInternalError, "failed to decode subscribe error response");
            }
            return status_from_msg(err->status, err->message, ErrorCode::kInternalError);
        }
        if (type != detail::MsgType::kSubscribeResp) {
            return Status(ErrorCode::kInternalError, "unexpected subscribe response type");
        }

        auto resp = detail::decode_subscribe_response(response_payload);
        if (!resp.has_value()) {
            return Status(ErrorCode::kInternalError, "failed to decode subscribe response");
        }
        return status_from_msg(resp->status, resp->message, ErrorCode::kInternalError);
    }

    Status unsubscribe_remote(const std::string& key) {
        std::lock_guard<std::mutex> lock(control_mu_);
        if (control_fd_ < 0) {
            return Status(ErrorCode::kConnectionReset, "control connection is closed");
        }

        detail::UnsubscribeRequest req;
        req.subscriber_node_id = node_id_;
        req.key = key;

        const uint64_t request_id = next_request_id_.fetch_add(1);
        if (!detail::send_frame(control_fd_, detail::MsgType::kUnsubscribe, request_id, detail::encode(req))) {
            return Status(ErrorCode::kConnectionReset, "failed to send unsubscribe request");
        }

        detail::MsgHeader header;
        std::vector<uint8_t> response_payload;
        if (!detail::recv_frame(control_fd_, &header, &response_payload)) {
            return Status(ErrorCode::kConnectionReset, "failed to read unsubscribe response");
        }
        if (header.request_id != request_id) {
            return Status(ErrorCode::kInternalError, "unsubscribe response request_id mismatch");
        }

        const auto type = static_cast<detail::MsgType>(header.type);
        if (type == detail::MsgType::kError) {
            auto err = detail::decode_error_response(response_payload);
            if (!err.has_value()) {
                return Status(ErrorCode::kInternalError, "failed to decode unsubscribe error response");
            }
            return status_from_msg(err->status, err->message, ErrorCode::kInternalError);
        }
        if (type != detail::MsgType::kUnsubscribeResp) {
            return Status(ErrorCode::kInternalError, "unexpected unsubscribe response type");
        }

        auto resp = detail::decode_unsubscribe_response(response_payload);
        if (!resp.has_value()) {
            return Status(ErrorCode::kInternalError, "failed to decode unsubscribe response");
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

    Status handle_push_commit(const detail::PushCommitRequest& req) {
        {
            std::lock_guard<std::mutex> push_lock(push_mu_);
            if (push_busy_) {
                return Status(ErrorCode::kConnectionRefused, "push inbox busy");
            }
            push_busy_ = true;
        }
        struct ResetBusy {
            Impl* self;
            ~ResetBusy() {
                std::lock_guard<std::mutex> lock(self->push_mu_);
                self->push_busy_ = false;
            }
        } reset{this};

        if (!push_inbox_region_ || req.target_node_id != node_id_) {
            return Status(ErrorCode::kInvalidArgument, "invalid push target");
        }

        if (push_inbox_capacity_ < sizeof(PushInboxHeader)) {
            return Status(ErrorCode::kInternalError, "push inbox not initialized");
        }

        PushInboxHeader header;
        std::memcpy(&header, push_inbox_region_->address(), sizeof(header));
        if (header.magic != kPushInboxMagic) {
            return Status(ErrorCode::kInvalidArgument, "invalid push inbox header");
        }
        if (header.value_size != req.value_size) {
            return Status(ErrorCode::kInvalidArgument, "push value size mismatch");
        }
        const size_t total_size = sizeof(PushInboxHeader) + header.key_size +
                                  static_cast<size_t>(header.value_size);
        if (total_size > push_inbox_capacity_) {
            return Status(ErrorCode::kInvalidArgument, "push payload exceeds inbox capacity");
        }

        const auto* base = static_cast<const uint8_t*>(push_inbox_region_->address());
        const auto* key_ptr = base + sizeof(PushInboxHeader);
        const auto* value_ptr = key_ptr + header.key_size;
        std::string inbox_key(reinterpret_cast<const char*>(key_ptr), header.key_size);
        if (inbox_key != req.key) {
            return Status(ErrorCode::kInvalidArgument, "push key mismatch");
        }

        auto region = MemoryRegion::allocate(context_, static_cast<size_t>(header.value_size));
        if (!region) {
            return Status(ErrorCode::kRegistrationFailed, "failed to allocate push finalize region");
        }
        std::memcpy(region->address(), value_ptr, static_cast<size_t>(header.value_size));

        auto publish = publish_impl(req.key, region, static_cast<size_t>(header.value_size), nullptr);
        return publish.status();
    }

    void serve_push_connection(int fd) {
        detail::MsgHeader header;
        std::vector<uint8_t> payload;
        if (!detail::recv_frame(fd, &header, &payload)) {
            detail::TcpTransport::close_fd(&fd);
            return;
        }

        if (static_cast<detail::MsgType>(header.type) != detail::MsgType::kPushCommit) {
            auto err = detail::encode(detail::ErrorResponse{detail::MsgStatus::kInvalidRequest, "unsupported push message"});
            (void)detail::send_frame(fd, detail::MsgType::kError, header.request_id, err);
            detail::TcpTransport::close_fd(&fd);
            return;
        }

        auto req = detail::decode_push_commit_request(payload);
        if (!req.has_value()) {
            auto err = detail::encode(detail::ErrorResponse{detail::MsgStatus::kInvalidRequest, "bad push commit request"});
            (void)detail::send_frame(fd, detail::MsgType::kError, header.request_id, err);
            detail::TcpTransport::close_fd(&fd);
            return;
        }

        detail::PushCommitResponse resp;
        auto status = handle_push_commit(*req);
        resp.status = status.ok() ? detail::MsgStatus::kOk : detail::MsgStatus::kInvalidRequest;
        resp.message = status.ok() ? std::string{} : status.message();
        auto bytes = detail::encode(resp);
        (void)detail::send_frame(fd, detail::MsgType::kPushCommitResp, header.request_id, bytes);
        detail::TcpTransport::close_fd(&fd);
    }

    void push_accept_loop() {
        while (running_.load()) {
            std::string error;
            auto conn = detail::TcpTransport::accept(push_listen_fd_, &error);
            if (conn.fd < 0) {
                if (!running_.load()) {
                    break;
                }
                continue;
            }
            serve_push_connection(conn.fd);
        }
    }

    void serve_subscription_connection(int fd) {
        detail::MsgHeader header;
        std::vector<uint8_t> payload;
        if (!detail::recv_frame(fd, &header, &payload)) {
            detail::TcpTransport::close_fd(&fd);
            return;
        }

        if (static_cast<detail::MsgType>(header.type) != detail::MsgType::kSubscriptionEvent) {
            detail::TcpTransport::close_fd(&fd);
            return;
        }

        auto event = detail::decode_subscription_event(payload);
        if (event.has_value()) {
            SubscriptionEvent local;
            switch (event->type) {
                case detail::SubscriptionEventType::kPublished:
                    local.type = SubscriptionEventType::kPublished;
                    break;
                case detail::SubscriptionEventType::kUpdated:
                    local.type = SubscriptionEventType::kUpdated;
                    break;
                case detail::SubscriptionEventType::kUnpublished:
                    local.type = SubscriptionEventType::kUnpublished;
                    break;
                case detail::SubscriptionEventType::kOwnerLost:
                    local.type = SubscriptionEventType::kOwnerLost;
                    break;
            }
            local.key = std::move(event->key);
            local.owner_node_id = std::move(event->owner_node_id);
            local.version = event->version;

            std::lock_guard<std::mutex> lock(subscription_mu_);
            subscription_events_.push_back(std::move(local));
        }

        detail::TcpTransport::close_fd(&fd);
    }

    void subscription_accept_loop() {
        while (running_.load()) {
            std::string error;
            auto conn = detail::TcpTransport::accept(subscription_listen_fd_, &error);
            if (conn.fd < 0) {
                if (!running_.load()) {
                    break;
                }
                continue;
            }
            serve_subscription_connection(conn.fd);
        }
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
    std::string error;
    impl_->push_inbox_region_ = MemoryRegion::allocate(impl_->context_, impl_->push_inbox_capacity_);
    if (!impl_->push_inbox_region_) {
        impl_->listener_->close();
        impl_->listener_.reset();
        impl_->worker_.reset();
        impl_->context_.reset();
        impl_->node_id_.clear();
        return Status(ErrorCode::kRegistrationFailed, "failed to allocate push inbox region");
    }
    impl_->push_inbox_rkey_ = impl_->push_inbox_region_->remote_key();
    impl_->push_listen_fd_ = detail::TcpTransport::listen(
        make_ephemeral_bind_addr(impl_->local_data_addr_), &impl_->push_control_addr_, &error);
    if (impl_->push_listen_fd_ < 0) {
        impl_->listener_->close();
        impl_->listener_.reset();
        impl_->worker_.reset();
        impl_->context_.reset();
        impl_->push_inbox_region_.reset();
        impl_->node_id_.clear();
        return Status(ErrorCode::kConnectionRefused, error.empty() ? "failed to create push control listener" : error);
    }
    impl_->subscription_listen_fd_ = detail::TcpTransport::listen(
        make_ephemeral_bind_addr(impl_->local_data_addr_), &impl_->subscription_control_addr_, &error);
    if (impl_->subscription_listen_fd_ < 0) {
        detail::TcpTransport::close_fd(&impl_->push_listen_fd_);
        impl_->listener_->close();
        impl_->listener_.reset();
        impl_->worker_.reset();
        impl_->context_.reset();
        impl_->push_inbox_region_.reset();
        impl_->node_id_.clear();
        return Status(ErrorCode::kConnectionRefused,
                      error.empty() ? "failed to create subscription listener" : error);
    }
    impl_->running_.store(true);
    impl_->push_accept_thread_ = std::thread([impl = impl_.get()]() { impl->push_accept_loop(); });
    impl_->subscription_accept_thread_ =
        std::thread([impl = impl_.get()]() { impl->subscription_accept_loop(); });
    impl_->worker_->start_progress_thread();

    impl_->control_fd_ = detail::TcpTransport::connect(impl_->server_addr_, &error);
    if (impl_->control_fd_ < 0) {
        detail::TcpTransport::close_fd(&impl_->push_listen_fd_);
        detail::TcpTransport::close_fd(&impl_->subscription_listen_fd_);
        if (impl_->push_accept_thread_.joinable()) {
            impl_->push_accept_thread_.join();
        }
        if (impl_->subscription_accept_thread_.joinable()) {
            impl_->subscription_accept_thread_.join();
        }
        impl_->running_.store(false);
        impl_->listener_->close();
        impl_->listener_.reset();
        impl_->worker_->stop_progress_thread();
        impl_->worker_.reset();
        impl_->context_.reset();
        impl_->push_inbox_region_.reset();
        impl_->node_id_.clear();
        return Status(ErrorCode::kConnectionRefused, error.empty() ? "failed to connect to KV server" : error);
    }

    auto status = impl_->register_with_server();
    if (!status.ok()) {
        detail::TcpTransport::close_fd(&impl_->control_fd_);
        detail::TcpTransport::close_fd(&impl_->push_listen_fd_);
        detail::TcpTransport::close_fd(&impl_->subscription_listen_fd_);
        if (impl_->push_accept_thread_.joinable()) {
            impl_->push_accept_thread_.join();
        }
        if (impl_->subscription_accept_thread_.joinable()) {
            impl_->subscription_accept_thread_.join();
        }
        impl_->running_.store(false);
        impl_->listener_->close();
        impl_->listener_.reset();
        impl_->worker_->stop_progress_thread();
        impl_->worker_.reset();
        impl_->context_.reset();
        impl_->push_inbox_region_.reset();
        impl_->node_id_.clear();
        return status;
    }
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
    detail::TcpTransport::close_fd(&impl_->push_listen_fd_);
    detail::TcpTransport::close_fd(&impl_->subscription_listen_fd_);
    if (impl_->push_accept_thread_.joinable()) {
        impl_->push_accept_thread_.join();
    }
    if (impl_->subscription_accept_thread_.joinable()) {
        impl_->subscription_accept_thread_.join();
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
    impl_->push_inbox_region_.reset();
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

std::optional<PushMetrics> KVNode::last_push_metrics() const {
    if (!impl_) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(impl_->metrics_mu_);
    return impl_->last_push_metrics_;
}

std::vector<SubscriptionEvent> KVNode::drain_subscription_events() {
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->subscription_mu_);
    std::vector<SubscriptionEvent> drained;
    drained.swap(impl_->subscription_events_);
    return drained;
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

Future<void> KVNode::push(const std::string& target_node_id,
                          const std::string& key,
                          const void* data,
                          size_t size) {
    if (!impl_) {
        return Future<void>::make_error(Status(ErrorCode::kInternalError, "KVNode not initialized"));
    }
    PushMetrics metrics;
    const auto total_start = SteadyClock::now();
    if (!impl_->running_.load()) {
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_push_metrics(metrics);
        return Future<void>::make_error(Status(ErrorCode::kConnectionReset, "KVNode is not running"));
    }
    if (target_node_id.empty() || key.empty() || !data || size == 0) {
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_push_metrics(metrics);
        return Future<void>::make_error(Status(ErrorCode::kInvalidArgument, "invalid push arguments"));
    }

    Status lookup_status;
    const auto lookup_start = SteadyClock::now();
    auto target = impl_->get_push_target(target_node_id, &lookup_status);
    metrics.get_target_rpc_us = elapsed_us(lookup_start, SteadyClock::now());
    if (!target.has_value()) {
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_push_metrics(metrics);
        return Future<void>::make_error(lookup_status);
    }

    const size_t frame_size = sizeof(PushInboxHeader) + key.size() + size;
    if (frame_size > target->push_inbox_capacity) {
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_push_metrics(metrics);
        return Future<void>::make_error(Status(ErrorCode::kInvalidArgument, "push payload exceeds target inbox capacity"));
    }

    const auto prepare_start = SteadyClock::now();
    auto local_region = MemoryRegion::allocate(impl_->context_, frame_size);
    if (!local_region) {
        metrics.prepare_frame_us = elapsed_us(prepare_start, SteadyClock::now());
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_push_metrics(metrics);
        return Future<void>::make_error(Status(ErrorCode::kRegistrationFailed, "failed to allocate push frame region"));
    }

    PushInboxHeader header;
    header.key_size = static_cast<uint32_t>(key.size());
    header.value_size = static_cast<uint64_t>(size);
    auto* base = static_cast<uint8_t*>(local_region->address());
    std::memcpy(base, &header, sizeof(header));
    std::memcpy(base + sizeof(header), key.data(), key.size());
    std::memcpy(base + sizeof(header) + key.size(), data, size);
    metrics.prepare_frame_us = elapsed_us(prepare_start, SteadyClock::now());

    Status peer_status;
    auto peer = impl_->get_or_connect_peer(target->target_node_id, target->target_data_addr, &peer_status);
    if (!peer) {
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_push_metrics(metrics);
        return Future<void>::make_error(peer_status);
    }

    RemoteKey rkey;
    rkey.data = target->push_inbox_rkey;
    const auto rdma_start = SteadyClock::now();
    auto put = peer->put(local_region, 0, target->push_inbox_remote_addr, rkey, frame_size);
    put.get();
    if (!put.status().ok()) {
        metrics.rdma_put_flush_us = elapsed_us(rdma_start, SteadyClock::now());
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_push_metrics(metrics);
        return Future<void>::make_error(put.status());
    }
    // UCX put completion does not guarantee remote inbox visibility before commit handling.
    auto flush = peer->flush();
    flush.get();
    metrics.rdma_put_flush_us = elapsed_us(rdma_start, SteadyClock::now());
    if (!flush.status().ok()) {
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_push_metrics(metrics);
        return Future<void>::make_error(flush.status());
    }

    std::string error;
    const auto commit_start = SteadyClock::now();
    int fd = detail::TcpTransport::connect(target->push_control_addr, &error);
    if (fd < 0) {
        metrics.commit_rpc_us = elapsed_us(commit_start, SteadyClock::now());
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_push_metrics(metrics);
        return Future<void>::make_error(Status(ErrorCode::kConnectionRefused, error.empty() ? "failed to connect to target push control" : error));
    }

    detail::PushCommitRequest req;
    req.target_node_id = target_node_id;
    req.sender_node_id = impl_->node_id_;
    req.key = key;
    req.value_size = static_cast<uint64_t>(size);
    const uint64_t request_id = impl_->next_request_id_.fetch_add(1);
    if (!detail::send_frame(fd, detail::MsgType::kPushCommit, request_id, detail::encode(req))) {
        detail::TcpTransport::close_fd(&fd);
        metrics.commit_rpc_us = elapsed_us(commit_start, SteadyClock::now());
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_push_metrics(metrics);
        return Future<void>::make_error(Status(ErrorCode::kConnectionReset, "failed to send push commit"));
    }

    detail::MsgHeader resp_header;
    std::vector<uint8_t> resp_payload;
    if (!detail::recv_frame(fd, &resp_header, &resp_payload)) {
        detail::TcpTransport::close_fd(&fd);
        metrics.commit_rpc_us = elapsed_us(commit_start, SteadyClock::now());
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_push_metrics(metrics);
        return Future<void>::make_error(Status(ErrorCode::kConnectionReset, "failed to read push commit response"));
    }
    detail::TcpTransport::close_fd(&fd);
    metrics.commit_rpc_us = elapsed_us(commit_start, SteadyClock::now());
    if (resp_header.request_id != request_id) {
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_push_metrics(metrics);
        return Future<void>::make_error(Status(ErrorCode::kInternalError, "push commit response request_id mismatch"));
    }
    if (static_cast<detail::MsgType>(resp_header.type) != detail::MsgType::kPushCommitResp) {
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_push_metrics(metrics);
        return Future<void>::make_error(Status(ErrorCode::kInternalError, "unexpected push commit response type"));
    }
    auto resp = detail::decode_push_commit_response(resp_payload);
    if (!resp.has_value()) {
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_push_metrics(metrics);
        return Future<void>::make_error(Status(ErrorCode::kInternalError, "failed to decode push commit response"));
    }
    auto status = status_from_msg(resp->status, resp->message, ErrorCode::kInternalError);
    if (!status.ok()) {
        metrics.total_us = elapsed_us(total_start, SteadyClock::now());
        metrics.ok = false;
        impl_->record_push_metrics(metrics);
        return Future<void>::make_error(status);
    }
    metrics.total_us = elapsed_us(total_start, SteadyClock::now());
    metrics.ok = true;
    impl_->record_push_metrics(metrics);
    return Future<void>::make_ready();
}

Future<void> KVNode::subscribe(const std::string& key) {
    if (!impl_ || !impl_->running_.load()) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument, "KVNode is not running"));
    }
    if (key.empty()) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument, "subscribe key must not be empty"));
    }
    auto status = impl_->subscribe_remote(key);
    if (!status.ok()) {
        return Future<void>::make_error(status);
    }
    return Future<void>::make_ready();
}

Future<void> KVNode::unsubscribe(const std::string& key) {
    if (!impl_ || !impl_->running_.load()) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument, "KVNode is not running"));
    }
    if (key.empty()) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument, "unsubscribe key must not be empty"));
    }
    auto status = impl_->unsubscribe_remote(key);
    if (!status.ok()) {
        return Future<void>::make_error(status);
    }
    return Future<void>::make_ready();
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
