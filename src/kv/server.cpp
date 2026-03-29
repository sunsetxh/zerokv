#include "axon/kv.h"

#include "kv/metadata_store.h"
#include "kv/protocol.h"
#include "kv/tcp_framing.h"
#include "kv/tcp_transport.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace axon::kv {

namespace {

detail::RegisterNodeResponse register_node_response(detail::MsgStatus status,
                                                    std::string assigned_node_id = {},
                                                    std::string message = {}) {
    detail::RegisterNodeResponse resp;
    resp.status = status;
    resp.assigned_node_id = std::move(assigned_node_id);
    resp.message = std::move(message);
    return resp;
}

detail::PutMetaResponse put_meta_response(detail::MsgStatus status, std::string message = {}) {
    detail::PutMetaResponse resp;
    resp.status = status;
    resp.message = std::move(message);
    return resp;
}

detail::GetMetaResponse get_meta_response(detail::MsgStatus status,
                                          std::optional<detail::KeyMetadata> metadata = std::nullopt,
                                          std::string message = {}) {
    detail::GetMetaResponse resp;
    resp.status = status;
    resp.metadata = std::move(metadata);
    resp.message = std::move(message);
    return resp;
}

detail::UnpublishResponse unpublish_response(detail::MsgStatus status, std::string message = {}) {
    detail::UnpublishResponse resp;
    resp.status = status;
    resp.message = std::move(message);
    return resp;
}

detail::ErrorResponse error_response(detail::MsgStatus status, std::string message) {
    detail::ErrorResponse resp;
    resp.status = status;
    resp.message = std::move(message);
    return resp;
}

}  // namespace

struct KVServer::Impl {
    explicit Impl(const axon::Config& cfg) : cfg_(cfg) {}

    struct Session {
        int fd = -1;
        std::thread thread;
        std::string node_id;
    };

    Config cfg_;
    detail::MetadataStore store_;
    std::atomic<bool> running_{false};
    int listen_fd_ = -1;
    std::string bound_address_;
    std::thread accept_thread_;

    std::mutex sessions_mu_;
    std::unordered_map<uint64_t, std::unique_ptr<Session>> sessions_;
    std::atomic<uint64_t> next_session_id_{1};

    Status handle_register_node(const detail::RegisterNodeRequest& req,
                                detail::RegisterNodeResponse& resp) {
        detail::NodeInfo node;
        node.node_id = req.node_id;
        node.control_addr = req.control_addr;
        node.data_addr = req.data_addr;
        node.last_heartbeat_ms = 0;
        node.state = detail::NodeInfo::State::kAlive;
        if (!store_.register_node(std::move(node))) {
            resp = register_node_response(detail::MsgStatus::kInvalidRequest, {}, "invalid node registration");
            return Status(ErrorCode::kInvalidArgument, "invalid node registration");
        }
        resp = register_node_response(detail::MsgStatus::kOk, req.node_id);
        return Status::OK();
    }

    Status handle_put_meta(const detail::PutMetaRequest& req, detail::PutMetaResponse& resp) {
        auto meta = req.metadata;
        if (meta.version == 0) {
            meta.version = 1;
        }
        if (!store_.put(std::move(meta))) {
            resp = put_meta_response(detail::MsgStatus::kInvalidRequest, "metadata rejected");
            return Status(ErrorCode::kInvalidArgument, "metadata rejected");
        }
        resp = put_meta_response(detail::MsgStatus::kOk);
        return Status::OK();
    }

    Status handle_get_meta(const detail::GetMetaRequest& req, detail::GetMetaResponse& resp) {
        auto meta = store_.get(req.key);
        if (!meta.has_value()) {
            resp = get_meta_response(detail::MsgStatus::kNotFound, std::nullopt, "key not found");
            return Status(ErrorCode::kInvalidArgument, "key not found");
        }
        auto node = store_.get_node(meta->owner_node_id);
        if (!node.has_value() || node->state != detail::NodeInfo::State::kAlive) {
            resp = get_meta_response(detail::MsgStatus::kStaleOwner, std::nullopt, "owner unavailable");
            return Status(ErrorCode::kConnectionReset, "owner unavailable");
        }
        resp = get_meta_response(detail::MsgStatus::kOk, meta);
        return Status::OK();
    }

    Status handle_unpublish(const detail::UnpublishRequest& req, detail::UnpublishResponse& resp) {
        if (!store_.erase(req.key, req.owner_node_id)) {
            resp = unpublish_response(detail::MsgStatus::kNotFound, "key not owned");
            return Status(ErrorCode::kInvalidArgument, "key not owned");
        }
        resp = unpublish_response(detail::MsgStatus::kOk);
        return Status::OK();
    }

    Status handle_heartbeat(const detail::HeartbeatRequest& req) {
        if (!store_.update_heartbeat(req.node_id, req.timestamp_ms)) {
            return Status(ErrorCode::kInvalidArgument, "unknown node");
        }
        return Status::OK();
    }

    void serve_session(uint64_t session_id, int fd) {
        while (running_.load()) {
            detail::MsgHeader header;
            std::vector<uint8_t> payload;
            if (!detail::recv_frame(fd, &header, &payload)) {
                break;
            }

            switch (static_cast<detail::MsgType>(header.type)) {
                case detail::MsgType::kRegisterNode: {
                    auto req = detail::decode_register_node_request(payload);
                    if (!req.has_value()) {
                        auto err = detail::encode(error_response(detail::MsgStatus::kInvalidRequest, "bad register request"));
                        (void)detail::send_frame(fd, detail::MsgType::kError, header.request_id, err);
                        break;
                    }
                    detail::RegisterNodeResponse resp;
                    Status st = handle_register_node(*req, resp);
                    if (st.ok()) {
                        std::lock_guard<std::mutex> lock(sessions_mu_);
                        auto it = sessions_.find(session_id);
                        if (it != sessions_.end()) {
                            it->second->node_id = req->node_id;
                        }
                    }
                    auto bytes = detail::encode(resp);
                    (void)detail::send_frame(fd, detail::MsgType::kRegisterNodeResp, header.request_id, bytes);
                    break;
                }
                case detail::MsgType::kPutMeta: {
                    auto req = detail::decode_put_meta_request(payload);
                    if (!req.has_value()) {
                        auto err = detail::encode(error_response(detail::MsgStatus::kInvalidRequest, "bad put_meta request"));
                        (void)detail::send_frame(fd, detail::MsgType::kError, header.request_id, err);
                        break;
                    }
                    detail::PutMetaResponse resp;
                    (void)handle_put_meta(*req, resp);
                    auto bytes = detail::encode(resp);
                    (void)detail::send_frame(fd, detail::MsgType::kPutMetaResp, header.request_id, bytes);
                    break;
                }
                case detail::MsgType::kGetMeta: {
                    auto req = detail::decode_get_meta_request(payload);
                    if (!req.has_value()) {
                        auto err = detail::encode(error_response(detail::MsgStatus::kInvalidRequest, "bad get_meta request"));
                        (void)detail::send_frame(fd, detail::MsgType::kError, header.request_id, err);
                        break;
                    }
                    detail::GetMetaResponse resp;
                    (void)handle_get_meta(*req, resp);
                    auto bytes = detail::encode(resp);
                    (void)detail::send_frame(fd, detail::MsgType::kGetMetaResp, header.request_id, bytes);
                    break;
                }
                case detail::MsgType::kUnpublish: {
                    auto req = detail::decode_unpublish_request(payload);
                    if (!req.has_value()) {
                        auto err = detail::encode(error_response(detail::MsgStatus::kInvalidRequest, "bad unpublish request"));
                        (void)detail::send_frame(fd, detail::MsgType::kError, header.request_id, err);
                        break;
                    }
                    detail::UnpublishResponse resp;
                    (void)handle_unpublish(*req, resp);
                    auto bytes = detail::encode(resp);
                    (void)detail::send_frame(fd, detail::MsgType::kUnpublishResp, header.request_id, bytes);
                    break;
                }
                case detail::MsgType::kHeartbeat: {
                    auto req = detail::decode_heartbeat_request(payload);
                    if (req.has_value()) {
                        (void)handle_heartbeat(*req);
                    }
                    break;
                }
                default: {
                    auto err = detail::encode(error_response(detail::MsgStatus::kInvalidRequest, "unsupported message"));
                    (void)detail::send_frame(fd, detail::MsgType::kError, header.request_id, err);
                    break;
                }
            }
        }

        std::string node_id;
        {
            std::lock_guard<std::mutex> lock(sessions_mu_);
            auto it = sessions_.find(session_id);
            if (it != sessions_.end()) {
                node_id = it->second->node_id;
            }
        }
        if (!node_id.empty()) {
            (void)store_.mark_node_dead(node_id);
        }
        detail::TcpTransport::close_fd(&fd);
    }

    void accept_loop() {
        while (running_.load()) {
            std::string error;
            auto conn = detail::TcpTransport::accept(listen_fd_, &error);
            if (conn.fd < 0) {
                if (!running_.load()) {
                    break;
                }
                continue;
            }

            const uint64_t session_id = next_session_id_++;
            auto session = std::make_unique<Session>();
            session->fd = conn.fd;
            session->thread = std::thread([this, session_id, fd = conn.fd]() { serve_session(session_id, fd); });

            std::lock_guard<std::mutex> lock(sessions_mu_);
            sessions_.emplace(session_id, std::move(session));
        }
    }
};

KVServer::KVServer(const axon::Config& cfg) : impl_(std::make_unique<Impl>(cfg)) {}

KVServer::~KVServer() {
    stop();
}

KVServer::Ptr KVServer::create(const axon::Config& cfg) {
    return Ptr(new KVServer(cfg));
}

Status KVServer::start(const ServerConfig& cfg) {
    if (impl_->running_.load()) {
        return Status::OK();
    }

    std::string error;
    impl_->listen_fd_ = detail::TcpTransport::listen(cfg.listen_addr, &impl_->bound_address_, &error);
    if (impl_->listen_fd_ < 0) {
        return Status(ErrorCode::kTransportError, error);
    }

    impl_->running_.store(true);
    impl_->accept_thread_ = std::thread([this]() { impl_->accept_loop(); });
    return Status::OK();
}

void KVServer::stop() {
    if (!impl_ || !impl_->running_.exchange(false)) {
        return;
    }

    detail::TcpTransport::close_fd(&impl_->listen_fd_);
    if (impl_->accept_thread_.joinable()) {
        impl_->accept_thread_.join();
    }

    std::unordered_map<uint64_t, std::unique_ptr<Impl::Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(impl_->sessions_mu_);
        sessions.swap(impl_->sessions_);
    }
    for (auto& [id, session] : sessions) {
        detail::TcpTransport::close_fd(&session->fd);
        if (session->thread.joinable()) {
            session->thread.join();
        }
    }
}

bool KVServer::is_running() const noexcept {
    return impl_ && impl_->running_.load();
}

std::string KVServer::address() const {
    return impl_ ? impl_->bound_address_ : std::string{};
}

std::optional<KeyInfo> KVServer::lookup(const std::string& key) const {
    if (!impl_) {
        return std::nullopt;
    }
    auto metadata = impl_->store_.get(key);
    if (!metadata.has_value()) {
        return std::nullopt;
    }
    KeyInfo info;
    info.key = metadata->key;
    info.size = metadata->size;
    info.version = metadata->version;
    return info;
}

std::vector<std::string> KVServer::list_keys() const {
    return impl_ ? impl_->store_.list_keys() : std::vector<std::string>{};
}

}  // namespace axon::kv
