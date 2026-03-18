#include "axon/cluster.h"

#include "axon/endpoint.h"
#include "axon/worker.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace axon {

namespace {

constexpr uint32_t kControlMagic = 0x50325031U;  // "AXON1"
constexpr uint16_t kControlHeaderVersion = 1;
constexpr uint32_t kMaxControlPayloadSize = 1u << 20;

enum class ControlMessageType : uint16_t {
    kRegisterRequest = 1,
    kRegisterResponse = 2,
    kMembershipUpdate = 3,
};

struct ControlFrameHeader {
    uint32_t magic = kControlMagic;
    uint16_t header_version = kControlHeaderVersion;
    uint16_t type = 0;
    uint32_t payload_size = 0;
    uint32_t reserved = 0;
};

struct RegisterResponse {
    Status status = Status::OK();
    uint32_t assigned_rank = 0;
};

struct ParsedAddress {
    std::string host;
    uint16_t port = 0;
};

bool read_exact(int fd, void* buffer, size_t length) {
    auto* ptr = static_cast<std::byte*>(buffer);
    size_t total = 0;
    while (total < length) {
        const auto rc = ::recv(fd, ptr + total, length - total, 0);
        if (rc == 0) {
            return false;
        }
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        total += static_cast<size_t>(rc);
    }
    return true;
}

bool write_exact(int fd, const void* buffer, size_t length) {
    const auto* ptr = static_cast<const std::byte*>(buffer);
    size_t total = 0;
    while (total < length) {
        const auto rc = ::send(fd, ptr + total, length - total, MSG_NOSIGNAL);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        total += static_cast<size_t>(rc);
    }
    return true;
}

void shutdown_and_close(int* fd) {
    if (!fd || *fd < 0) {
        return;
    }
    (void)::shutdown(*fd, SHUT_RDWR);
    (void)::close(*fd);
    *fd = -1;
}

bool parse_address(const std::string& address, ParsedAddress* out, std::string* error) {
    const auto pos = address.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= address.size()) {
        if (error) {
            *error = "Invalid control address";
        }
        return false;
    }

    const auto host = address.substr(0, pos);
    const auto port_str = address.substr(pos + 1);
    uint64_t port = 0;
    for (char c : port_str) {
        if (c < '0' || c > '9') {
            if (error) {
                *error = "Invalid control port";
            }
            return false;
        }
        port = (port * 10) + static_cast<uint64_t>(c - '0');
        if (port > std::numeric_limits<uint16_t>::max()) {
            if (error) {
                *error = "Control port out of range";
            }
            return false;
        }
    }

    out->host = host;
    out->port = static_cast<uint16_t>(port);
    return true;
}

int create_listen_socket(const std::string& address, std::string* bound_address,
                         std::string* error) {
    ParsedAddress parsed;
    if (!parse_address(address, &parsed, error)) {
        return -1;
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        return -1;
    }

    int reuse = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(parsed.port);
    if (::inet_pton(AF_INET, parsed.host.c_str(), &addr.sin_addr) != 1) {
        if (error) {
            *error = "Invalid IPv4 control host";
        }
        ::close(fd);
        return -1;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 64) != 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        ::close(fd);
        return -1;
    }

    if (bound_address) {
        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
            char host_buf[INET_ADDRSTRLEN] = {0};
            if (::inet_ntop(AF_INET, &bound.sin_addr, host_buf, sizeof(host_buf))) {
                *bound_address = std::string(host_buf) + ":" +
                    std::to_string(static_cast<unsigned>(ntohs(bound.sin_port)));
            }
        }
    }

    return fd;
}

int connect_socket(const std::string& address, std::string* error) {
    ParsedAddress parsed;
    if (!parse_address(address, &parsed, error)) {
        return -1;
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(parsed.port);
    if (::inet_pton(AF_INET, parsed.host.c_str(), &addr.sin_addr) != 1) {
        if (error) {
            *error = "Invalid IPv4 control host";
        }
        ::close(fd);
        return -1;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        ::close(fd);
        return -1;
    }

    return fd;
}

void append_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void append_u64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void append_bytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& bytes) {
    append_u32(out, static_cast<uint32_t>(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void append_string(std::vector<uint8_t>& out, const std::string& value) {
    append_u32(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

void append_bool(std::vector<uint8_t>& out, bool value) {
    out.push_back(static_cast<uint8_t>(value ? 1 : 0));
}

bool read_u32(const std::vector<uint8_t>& in, size_t* offset, uint32_t* value) {
    if (*offset + 4 > in.size()) {
        return false;
    }
    *value = static_cast<uint32_t>(in[*offset]) |
             (static_cast<uint32_t>(in[*offset + 1]) << 8) |
             (static_cast<uint32_t>(in[*offset + 2]) << 16) |
             (static_cast<uint32_t>(in[*offset + 3]) << 24);
    *offset += 4;
    return true;
}

bool read_u64(const std::vector<uint8_t>& in, size_t* offset, uint64_t* value) {
    if (*offset + 8 > in.size()) {
        return false;
    }
    *value = 0;
    for (int i = 0; i < 8; ++i) {
        *value |= (static_cast<uint64_t>(in[*offset + i]) << (i * 8));
    }
    *offset += 8;
    return true;
}

bool read_bool(const std::vector<uint8_t>& in, size_t* offset, bool* value) {
    if (*offset + 1 > in.size()) {
        return false;
    }
    *value = in[*offset] != 0;
    *offset += 1;
    return true;
}

bool read_string(const std::vector<uint8_t>& in, size_t* offset, std::string* value) {
    uint32_t size = 0;
    if (!read_u32(in, offset, &size) || *offset + size > in.size()) {
        return false;
    }
    value->assign(reinterpret_cast<const char*>(in.data() + *offset), size);
    *offset += size;
    return true;
}

bool read_bytes(const std::vector<uint8_t>& in, size_t* offset, std::vector<uint8_t>* value) {
    uint32_t size = 0;
    if (!read_u32(in, offset, &size) || *offset + size > in.size()) {
        return false;
    }
    value->assign(in.begin() + static_cast<std::ptrdiff_t>(*offset),
                  in.begin() + static_cast<std::ptrdiff_t>(*offset + size));
    *offset += size;
    return true;
}

bool write_frame(int fd, ControlMessageType type, const std::vector<uint8_t>& payload) {
    ControlFrameHeader header;
    header.type = static_cast<uint16_t>(type);
    header.payload_size = static_cast<uint32_t>(payload.size());
    return write_exact(fd, &header, sizeof(header)) &&
           (payload.empty() || write_exact(fd, payload.data(), payload.size()));
}

bool read_frame(int fd, ControlMessageType* type, std::vector<uint8_t>* payload) {
    ControlFrameHeader header;
    if (!read_exact(fd, &header, sizeof(header))) {
        return false;
    }
    if (header.magic != kControlMagic || header.header_version != kControlHeaderVersion) {
        return false;
    }
    if (header.payload_size > kMaxControlPayloadSize) {
        return false;
    }
    try {
        payload->assign(header.payload_size, 0);
    } catch (const std::bad_alloc&) {
        return false;
    }
    if (header.payload_size > 0 &&
        !read_exact(fd, payload->data(), payload->size())) {
        return false;
    }
    *type = static_cast<ControlMessageType>(header.type);
    return true;
}

std::vector<uint8_t> serialize_peer(const PeerDescriptor& peer) {
    std::vector<uint8_t> out;
    append_u32(out, peer.rank);
    append_bool(out, peer.is_master);
    append_string(out, peer.alias);
    append_string(out, peer.node_id);
    append_bytes(out, peer.worker_addr);
    return out;
}

bool deserialize_peer(const std::vector<uint8_t>& payload, size_t* offset, PeerDescriptor* peer) {
    if (!read_u32(payload, offset, &peer->rank) ||
        !read_bool(payload, offset, &peer->is_master) ||
        !read_string(payload, offset, &peer->alias) ||
        !read_string(payload, offset, &peer->node_id) ||
        !read_bytes(payload, offset, &peer->worker_addr)) {
        return false;
    }
    return true;
}

std::vector<uint8_t> serialize_register_request(const PeerDescriptor& self) {
    return serialize_peer(self);
}

bool deserialize_register_request(const std::vector<uint8_t>& payload, PeerDescriptor* peer) {
    size_t offset = 0;
    if (!deserialize_peer(payload, &offset, peer)) {
        return false;
    }
    return offset == payload.size();
}

std::vector<uint8_t> serialize_register_response(const RegisterResponse& response) {
    std::vector<uint8_t> out;
    append_u32(out, static_cast<uint32_t>(response.status.code()));
    append_u32(out, response.assigned_rank);
    append_string(out, response.status.message());
    return out;
}

bool deserialize_register_response(const std::vector<uint8_t>& payload, RegisterResponse* response) {
    size_t offset = 0;
    uint32_t code = 0;
    std::string message;
    if (!read_u32(payload, &offset, &code) ||
        !read_u32(payload, &offset, &response->assigned_rank) ||
        !read_string(payload, &offset, &message) ||
        offset != payload.size()) {
        return false;
    }
    response->status = Status(static_cast<ErrorCode>(code), std::move(message));
    return true;
}

std::vector<uint8_t> serialize_membership(const MembershipSnapshot& snapshot) {
    std::vector<uint8_t> out;
    append_u64(out, snapshot.epoch);
    append_u32(out, static_cast<uint32_t>(snapshot.peers.size()));
    for (const auto& peer : snapshot.peers) {
        const auto encoded_peer = serialize_peer(peer);
        out.insert(out.end(), encoded_peer.begin(), encoded_peer.end());
    }
    return out;
}

bool deserialize_membership(const std::vector<uint8_t>& payload, MembershipSnapshot* snapshot) {
    size_t offset = 0;
    uint32_t count = 0;
    if (!read_u64(payload, &offset, &snapshot->epoch) ||
        !read_u32(payload, &offset, &count)) {
        return false;
    }
    snapshot->peers.clear();
    snapshot->peers.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        PeerDescriptor peer;
        if (!deserialize_peer(payload, &offset, &peer)) {
            return false;
        }
        snapshot->peers.push_back(std::move(peer));
    }
    return offset == payload.size();
}

const PeerDescriptor* find_peer(const MembershipSnapshotPtr& membership, std::string_view alias) {
    if (!membership) {
        return nullptr;
    }
    for (const auto& peer : membership->peers) {
        if (peer.alias == alias) {
            return &peer;
        }
    }
    return nullptr;
}

bool endpoint_connected(const Endpoint::Ptr& endpoint);

}  // namespace

struct Cluster::Impl {
    struct SlaveSession {
        std::atomic<int> fd{-1};
        uint32_t rank = 0;
        std::string alias;
        bool registered = false;
        std::mutex io_mutex;
        std::thread reader_thread;
    };

    Context::Ptr context_;
    Worker::Ptr worker_;
    std::atomic<ClusterState> state_{ClusterState::kInit};
    bool is_master_ = false;
    uint32_t self_rank_ = 0;
    std::string self_alias_;
    bool owns_progress_thread_ = false;
    MasterClusterConfig master_cfg_;
    SlaveClusterConfig slave_cfg_;
    MembershipSnapshotPtr membership_ = std::make_shared<MembershipSnapshot>();

    std::atomic<bool> stop_requested_{false};
    int listen_fd_ = -1;
    int control_fd_ = -1;
    std::string bound_control_addr_;

    mutable std::mutex membership_mutex_;
    mutable std::mutex sessions_mutex_;
    mutable std::mutex routes_mutex_;
    std::thread accept_thread_;
    std::thread control_reader_thread_;
    std::vector<std::shared_ptr<SlaveSession>> slave_sessions_;
    struct RouteState {
        PeerDescriptor peer;
        Endpoint::Ptr endpoint;
        Status last_error = Status(ErrorCode::kNotImplemented, "Route not connected");
    };
    std::unordered_map<std::string, RouteState> routes_;
    static void close_session(const std::shared_ptr<SlaveSession>& session) {
        if (!session) {
            return;
        }
        std::lock_guard<std::mutex> lock(session->io_mutex);
        int fd = session->fd.exchange(-1);
        if (fd < 0) {
            return;
        }
        (void)::shutdown(fd, SHUT_RDWR);
        (void)::close(fd);
    }

    static bool write_session_frame(const std::shared_ptr<SlaveSession>& session,
                                    ControlMessageType type,
                                    const std::vector<uint8_t>& payload) {
        if (!session) {
            return false;
        }
        std::lock_guard<std::mutex> lock(session->io_mutex);
        const int fd = session->fd.load();
        if (fd < 0) {
            return false;
        }
        if (write_frame(fd, type, payload)) {
            return true;
        }
        int expected = fd;
        if (session->fd.compare_exchange_strong(expected, -1)) {
            (void)::shutdown(fd, SHUT_RDWR);
            (void)::close(fd);
        }
        return false;
    }

    void refresh_routes(const MembershipSnapshotPtr& snapshot) {
        if (!snapshot) {
            return;
        }

        std::unordered_map<std::string, RouteState> existing;
        {
            std::lock_guard<std::mutex> lock(routes_mutex_);
            existing = std::move(routes_);
            routes_.clear();
        }

        std::unordered_map<std::string, RouteState> next_routes;
        for (const auto& peer : snapshot->peers) {
            if (peer.rank == self_rank_) {
                continue;
            }

            RouteState route;
            route.peer = peer;

            auto existing_it = existing.find(peer.alias);
            if (existing_it != existing.end() &&
                endpoint_connected(existing_it->second.endpoint)) {
                route.endpoint = existing_it->second.endpoint;
                route.last_error = Status::OK();
            } else {
                auto connect_future = worker_->connect(peer.worker_addr);
                if (connect_future.status().ok()) {
                    route.endpoint = connect_future.get();
                    route.last_error = Status::OK();
                } else {
                    route.last_error = connect_future.status();
                }
            }

            next_routes.emplace(peer.alias, std::move(route));
        }

        std::lock_guard<std::mutex> lock(routes_mutex_);
        routes_ = std::move(next_routes);
    }
};

namespace {

bool endpoint_connected(const Endpoint::Ptr& endpoint) {
    return endpoint && endpoint->is_connected();
}

}  // namespace

std::string rank_alias(uint32_t rank) {
    return "RANK" + std::to_string(rank);
}

std::optional<uint32_t> parse_rank_alias(std::string_view alias) noexcept {
    constexpr std::string_view kPrefix = "RANK";
    if (!alias.starts_with(kPrefix) || alias.size() == kPrefix.size()) {
        return std::nullopt;
    }

    uint64_t value = 0;
    for (char c : alias.substr(kPrefix.size())) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = (value * 10) + static_cast<uint64_t>(c - '0');
        if (value > std::numeric_limits<uint32_t>::max()) {
            return std::nullopt;
        }
    }
    return static_cast<uint32_t>(value);
}

Cluster::Cluster(Context::Ptr ctx, Worker::Ptr worker, bool is_master)
    : impl_(std::make_unique<Impl>()) {
    impl_->context_ = std::move(ctx);
    impl_->worker_ = std::move(worker);
    impl_->is_master_ = is_master;
}

Cluster::~Cluster() {
    (void)stop();
}

Cluster::Ptr Cluster::create_master(Context::Ptr ctx, Worker::Ptr worker,
                                    MasterClusterConfig cfg) {
    if (!ctx || !worker) {
        return nullptr;
    }

    auto cluster = Ptr(new Cluster(std::move(ctx), std::move(worker), true));
    cluster->impl_->self_rank_ = 0;
    cluster->impl_->self_alias_ = rank_alias(0);
    cluster->impl_->master_cfg_ = std::move(cfg);

    auto snapshot = std::make_shared<MembershipSnapshot>();
    snapshot->epoch = 1;
    snapshot->peers.push_back(PeerDescriptor{
        .rank = 0,
        .alias = cluster->impl_->self_alias_,
        .worker_addr = cluster->impl_->worker_->address(),
        .node_id = {},
        .is_master = true,
    });
    cluster->impl_->membership_ = snapshot;
    return cluster;
}

Cluster::Ptr Cluster::create_slave(Context::Ptr ctx, Worker::Ptr worker,
                                   SlaveClusterConfig cfg) {
    if (!ctx || !worker) {
        return nullptr;
    }
    if (cfg.rank == 0) {
        return nullptr;
    }

    auto cluster = Ptr(new Cluster(std::move(ctx), std::move(worker), false));
    cluster->impl_->self_rank_ = cfg.rank;
    cluster->impl_->self_alias_ = rank_alias(cfg.rank);
    cluster->impl_->slave_cfg_ = std::move(cfg);
    cluster->impl_->membership_ = std::make_shared<MembershipSnapshot>();
    return cluster;
}

Status Cluster::start() {
    if (impl_->state_.load() != ClusterState::kInit &&
        impl_->state_.load() != ClusterState::kStopped) {
        return Status(ErrorCode::kInvalidArgument, "Cluster already started");
    }

    impl_->stop_requested_.store(false);
    impl_->state_.store(ClusterState::kStarting);
    if (!impl_->worker_->is_progress_thread_running()) {
        impl_->worker_->start_progress_thread();
        impl_->owns_progress_thread_ = true;
    }

    if (impl_->is_master_) {
        std::string error;
        impl_->listen_fd_ = create_listen_socket(
            impl_->master_cfg_.control_bind_addr, &impl_->bound_control_addr_, &error);
        if (impl_->listen_fd_ < 0) {
            impl_->state_.store(ClusterState::kFailed);
            return Status(ErrorCode::kConnectionRefused, error);
        }

        impl_->accept_thread_ = std::thread([this]() {
            while (!impl_->stop_requested_.load()) {
                const int client_fd = ::accept(impl_->listen_fd_, nullptr, nullptr);
                if (client_fd < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (!impl_->stop_requested_.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds{10});
                    }
                    continue;
                }

                auto session = std::make_shared<Impl::SlaveSession>();
                session->fd.store(client_fd);
                {
                    std::lock_guard<std::mutex> lock(impl_->sessions_mutex_);
                    impl_->slave_sessions_.push_back(session);
                }

                session->reader_thread = std::thread([this, session]() {
                    ControlMessageType type{};
                    std::vector<uint8_t> payload;
                    PeerDescriptor peer;
                    const int initial_fd = session->fd.load();
                    if (initial_fd < 0 ||
                        !read_frame(initial_fd, &type, &payload) ||
                        type != ControlMessageType::kRegisterRequest ||
                        !deserialize_register_request(payload, &peer)) {
                        Impl::close_session(session);
                        return;
                    }

                    RegisterResponse response;
                    response.assigned_rank = peer.rank;
                    MembershipSnapshotPtr snapshot;

                    {
                        std::lock_guard<std::mutex> lock(impl_->membership_mutex_);
                        auto current = std::make_shared<MembershipSnapshot>(*impl_->membership_);
                        if (peer.rank == 0 || peer.alias.empty() ||
                            peer.alias != rank_alias(peer.rank)) {
                            response.status = Status(
                                ErrorCode::kInvalidArgument, "Invalid slave rank or alias");
                        } else {
                            bool duplicate = false;
                            for (const auto& existing : current->peers) {
                                if (existing.rank == peer.rank || existing.alias == peer.alias) {
                                    duplicate = true;
                                    break;
                                }
                            }
                            if (duplicate) {
                                response.status = Status(
                                    ErrorCode::kInvalidArgument, "Duplicate rank or alias");
                            } else {
                                current->epoch += 1;
                                current->peers.push_back(peer);
                                impl_->membership_ = current;
                                snapshot = current;
                                session->rank = peer.rank;
                                session->alias = peer.alias;
                                session->registered = true;
                                response.status = Status::OK();
                                if (current->peers.size() >= impl_->master_cfg_.min_cluster_size) {
                                    impl_->state_.store(ClusterState::kReady);
                                } else {
                                    impl_->state_.store(ClusterState::kWaitingMembership);
                                }
                            }
                        }
                    }

                    if (!Impl::write_session_frame(session,
                                                   ControlMessageType::kRegisterResponse,
                                                   serialize_register_response(response))) {
                        return;
                    }

                    if (response.status.ok()) {
                        impl_->refresh_routes(snapshot);
                        std::vector<std::shared_ptr<Impl::SlaveSession>> sessions;
                        {
                            std::lock_guard<std::mutex> lock(impl_->sessions_mutex_);
                            sessions = impl_->slave_sessions_;
                        }
                        for (const auto& other : sessions) {
                            if (other->fd.load() >= 0) {
                                (void)Impl::write_session_frame(
                                    other, ControlMessageType::kMembershipUpdate,
                                    serialize_membership(*snapshot));
                            }
                        }
                    }

                    while (!impl_->stop_requested_.load()) {
                        ControlMessageType next_type{};
                        std::vector<uint8_t> next_payload;
                        const int active_fd = session->fd.load();
                        if (active_fd < 0 ||
                            !read_frame(active_fd, &next_type, &next_payload)) {
                            break;
                        }
                    }

                    if (session->registered && !impl_->stop_requested_.load()) {
                        MembershipSnapshotPtr snapshot;
                        {
                            std::lock_guard<std::mutex> lock(impl_->membership_mutex_);
                            auto current = std::make_shared<MembershipSnapshot>(*impl_->membership_);
                            auto it = current->peers.begin();
                            while (it != current->peers.end()) {
                                if (it->rank == session->rank) {
                                    it = current->peers.erase(it);
                                } else {
                                    ++it;
                                }
                            }
                            current->epoch += 1;
                            impl_->membership_ = current;
                            snapshot = current;
                            if (current->peers.size() >= impl_->master_cfg_.min_cluster_size) {
                                impl_->state_.store(ClusterState::kReady);
                            } else {
                                impl_->state_.store(ClusterState::kWaitingMembership);
                            }
                        }
                        impl_->refresh_routes(snapshot);

                        std::vector<std::shared_ptr<Impl::SlaveSession>> sessions;
                        {
                            std::lock_guard<std::mutex> lock(impl_->sessions_mutex_);
                            sessions = impl_->slave_sessions_;
                        }
                        for (const auto& other : sessions) {
                            if (other.get() != session.get() && other->fd.load() >= 0) {
                                (void)Impl::write_session_frame(
                                    other, ControlMessageType::kMembershipUpdate,
                                    serialize_membership(*snapshot));
                            }
                        }
                    }

                    Impl::close_session(session);
                });
            }
        });

        {
            std::lock_guard<std::mutex> lock(impl_->membership_mutex_);
            if (impl_->membership_->peers.size() >= impl_->master_cfg_.min_cluster_size) {
                impl_->state_.store(ClusterState::kReady);
            } else {
                impl_->state_.store(ClusterState::kWaitingMembership);
            }
        }
        return Status::OK();
    }

    impl_->state_.store(ClusterState::kRegistering);

    std::string error;
    impl_->control_fd_ = connect_socket(impl_->slave_cfg_.master_control_addr, &error);
    if (impl_->control_fd_ < 0) {
        impl_->state_.store(ClusterState::kFailed);
        return Status(ErrorCode::kConnectionRefused, error);
    }

    PeerDescriptor self{
        .rank = impl_->self_rank_,
        .alias = impl_->self_alias_,
        .worker_addr = impl_->worker_->address(),
        .node_id = {},
        .is_master = false,
    };
    if (!write_frame(impl_->control_fd_, ControlMessageType::kRegisterRequest,
                     serialize_register_request(self))) {
        shutdown_and_close(&impl_->control_fd_);
        impl_->state_.store(ClusterState::kFailed);
        return Status(ErrorCode::kConnectionReset, "Failed to send register request");
    }

    ControlMessageType type{};
    std::vector<uint8_t> payload;
    RegisterResponse response;
    if (!read_frame(impl_->control_fd_, &type, &payload) ||
        type != ControlMessageType::kRegisterResponse ||
        !deserialize_register_response(payload, &response)) {
        shutdown_and_close(&impl_->control_fd_);
        impl_->state_.store(ClusterState::kFailed);
        return Status(ErrorCode::kConnectionReset, "Failed to read register response");
    }
    if (!response.status.ok()) {
        shutdown_and_close(&impl_->control_fd_);
        impl_->state_.store(ClusterState::kFailed);
        return response.status;
    }

    MembershipSnapshot initial;
    if (!read_frame(impl_->control_fd_, &type, &payload) ||
        type != ControlMessageType::kMembershipUpdate ||
        !deserialize_membership(payload, &initial)) {
        shutdown_and_close(&impl_->control_fd_);
        impl_->state_.store(ClusterState::kFailed);
        return Status(ErrorCode::kConnectionReset, "Failed to read initial membership");
    }

    {
        std::lock_guard<std::mutex> lock(impl_->membership_mutex_);
        impl_->membership_ = std::make_shared<MembershipSnapshot>(std::move(initial));
    }
    impl_->refresh_routes(membership());
    impl_->state_.store(ClusterState::kReady);

    impl_->control_reader_thread_ = std::thread([this]() {
        while (!impl_->stop_requested_.load()) {
            ControlMessageType next_type{};
            std::vector<uint8_t> next_payload;
            if (!read_frame(impl_->control_fd_, &next_type, &next_payload)) {
                break;
            }
            if (next_type != ControlMessageType::kMembershipUpdate) {
                continue;
            }

            MembershipSnapshot snapshot;
            if (!deserialize_membership(next_payload, &snapshot)) {
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(impl_->membership_mutex_);
                impl_->membership_ = std::make_shared<MembershipSnapshot>(std::move(snapshot));
            }
            impl_->refresh_routes(membership());
            impl_->state_.store(ClusterState::kReady);
        }

        if (!impl_->stop_requested_.load()) {
            impl_->state_.store(ClusterState::kDegraded);
        }
    });

    return Status::OK();
}

Status Cluster::stop() {
    const auto current = impl_->state_.load();
    if (current == ClusterState::kStopped || current == ClusterState::kInit) {
        impl_->state_.store(ClusterState::kStopped);
        return Status::OK();
    }

    impl_->stop_requested_.store(true);
    impl_->state_.store(ClusterState::kStopping);

    shutdown_and_close(&impl_->listen_fd_);
    shutdown_and_close(&impl_->control_fd_);

    if (impl_->accept_thread_.joinable()) {
        impl_->accept_thread_.join();
    }
    if (impl_->control_reader_thread_.joinable()) {
        impl_->control_reader_thread_.join();
    }

    std::vector<std::shared_ptr<Impl::SlaveSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(impl_->sessions_mutex_);
        sessions = impl_->slave_sessions_;
    }
    for (const auto& session : sessions) {
        Impl::close_session(session);
    }
    for (const auto& session : sessions) {
        if (session->reader_thread.joinable()) {
            session->reader_thread.join();
        }
    }
    {
        std::lock_guard<std::mutex> lock(impl_->routes_mutex_);
        impl_->routes_.clear();
    }
    if (impl_->owns_progress_thread_) {
        impl_->worker_->stop_progress_thread();
        impl_->owns_progress_thread_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->sessions_mutex_);
        impl_->slave_sessions_.clear();
    }

    impl_->state_.store(ClusterState::kStopped);
    return Status::OK();
}

ClusterState Cluster::state() const noexcept {
    return impl_->state_.load();
}

uint32_t Cluster::self_rank() const noexcept {
    return impl_->self_rank_;
}

std::string Cluster::self_alias() const {
    return impl_->self_alias_;
}

bool Cluster::is_master() const noexcept {
    return impl_->is_master_;
}

std::optional<uint32_t> Cluster::resolve_rank(std::string_view alias) const {
    if (alias == impl_->self_alias_) {
        return impl_->self_rank_;
    }

    std::lock_guard<std::mutex> lock(impl_->membership_mutex_);
    if (const auto* peer = find_peer(impl_->membership_, alias)) {
        return peer->rank;
    }
    return std::nullopt;
}

MembershipSnapshotPtr Cluster::membership() const {
    std::lock_guard<std::mutex> lock(impl_->membership_mutex_);
    return impl_->membership_;
}

std::optional<RouteHandle> Cluster::route(std::string_view alias) const {
    if (alias == impl_->self_alias_) {
        return RouteHandle{
            .rank = impl_->self_rank_,
            .alias = impl_->self_alias_,
            .connected = true,
            .last_error = Status::OK(),
        };
    }

    std::lock_guard<std::mutex> lock(impl_->routes_mutex_);
    auto it = impl_->routes_.find(std::string(alias));
    if (it == impl_->routes_.end()) {
        return std::nullopt;
    }

    RouteHandle handle;
    handle.rank = it->second.peer.rank;
    handle.alias = it->second.peer.alias;
    handle.connected = endpoint_connected(it->second.endpoint);
    handle.last_error = it->second.last_error;
    return handle;
}

Future<void> Cluster::wait_ready() {
    if (impl_->state_.load() == ClusterState::kReady) {
        return Future<void>::make_ready();
    }
    return Future<void>::make_error(
        Status(ErrorCode::kNotImplemented, "wait_ready is not implemented"));
}

Future<void> Cluster::wait_ready(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (impl_->state_.load() == ClusterState::kReady) {
            return Future<void>::make_ready();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    if (impl_->state_.load() == ClusterState::kReady) {
        return Future<void>::make_ready();
    }
    return Future<void>::make_error(
        Status(ErrorCode::kTimeout, "Timed out waiting for cluster readiness"));
}

Future<void> Cluster::send(std::string_view alias, const void* buffer, size_t length, Tag tag) {
    if ((!buffer && length > 0) || !resolve_rank(alias).has_value()) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument, "Unknown alias"));
    }
    if (impl_->state_.load() != ClusterState::kReady) {
        return Future<void>::make_error(
            Status(ErrorCode::kInProgress, "Cluster is not ready"));
    }

    Endpoint::Ptr endpoint;
    Status last_error = Status(ErrorCode::kConnectionReset, "Route not connected");
    {
        std::lock_guard<std::mutex> lock(impl_->routes_mutex_);
        auto it = impl_->routes_.find(std::string(alias));
        if (it != impl_->routes_.end()) {
            endpoint = it->second.endpoint;
            last_error = it->second.last_error;
        }
    }
    if (!endpoint_connected(endpoint)) {
        return Future<void>::make_error(last_error);
    }
    return endpoint->tag_send(buffer, length, tag);
}

Future<void> Cluster::send(uint32_t rank, const void* buffer, size_t length, Tag tag) {
    return send(rank_alias(rank), buffer, length, tag);
}

Future<std::pair<size_t, Tag>>
Cluster::recv_any(void* buffer, size_t length, Tag tag, Tag tag_mask) {
    return impl_->worker_->tag_recv(buffer, length, tag, tag_mask);
}

}  // namespace axon
