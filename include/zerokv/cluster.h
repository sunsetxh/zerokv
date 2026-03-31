#pragma once

/// @file axon/cluster.h
/// @brief Cluster membership and alias-routing API skeleton.

#include "zerokv/common.h"
#include "zerokv/config.h"
#include "zerokv/future.h"
#include "zerokv/worker.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace axon {

enum class ClusterState {
    kInit,
    kStarting,
    kRegistering,
    kWaitingMembership,
    kConnectingPeers,
    kReady,
    kDegraded,
    kStopping,
    kStopped,
    kFailed
};

struct PeerDescriptor {
    uint32_t rank = 0;
    std::string alias;
    std::vector<uint8_t> worker_addr;
    std::string node_id;
    bool is_master = false;
};

struct PeerRuntimeState {
    PeerDescriptor desc;
    uint64_t last_heartbeat_ms = 0;
    bool control_alive = true;
    bool data_alive = true;
    bool online = true;
};

struct MembershipSnapshot {
    uint64_t epoch = 0;
    std::vector<PeerDescriptor> peers;
};

using MembershipSnapshotPtr = std::shared_ptr<const MembershipSnapshot>;

struct RouteHandle {
    uint32_t rank = 0;
    std::string alias;
    bool connected = false;
    Status last_error = Status::OK();
};

struct MasterClusterConfig {
    std::string control_bind_addr;
    std::string data_bind_addr = "0.0.0.0:0";
    size_t min_cluster_size = 1;
    std::chrono::milliseconds heartbeat_interval{1000};
    std::chrono::milliseconds heartbeat_timeout{5000};
};

struct SlaveClusterConfig {
    uint32_t rank = 0;
    std::string master_control_addr;
    std::string data_bind_addr = "0.0.0.0:0";
    std::chrono::milliseconds heartbeat_interval{1000};
    std::chrono::milliseconds heartbeat_timeout{5000};
};

[[nodiscard]] std::string rank_alias(uint32_t rank);
[[nodiscard]] std::optional<uint32_t> parse_rank_alias(std::string_view alias) noexcept;

class Cluster : public std::enable_shared_from_this<Cluster> {
public:
    using Ptr = std::shared_ptr<Cluster>;

    static Ptr create_master(Context::Ptr ctx, Worker::Ptr worker,
                             MasterClusterConfig cfg);
    static Ptr create_slave(Context::Ptr ctx, Worker::Ptr worker,
                            SlaveClusterConfig cfg);

    ~Cluster();
    Cluster(const Cluster&) = delete;
    Cluster& operator=(const Cluster&) = delete;

    Status start();
    Status stop();

    [[nodiscard]] ClusterState state() const noexcept;
    [[nodiscard]] uint32_t self_rank() const noexcept;
    [[nodiscard]] std::string self_alias() const;
    [[nodiscard]] bool is_master() const noexcept;

    [[nodiscard]] std::optional<uint32_t> resolve_rank(std::string_view alias) const;
    [[nodiscard]] MembershipSnapshotPtr membership() const;
    [[nodiscard]] std::optional<RouteHandle> route(std::string_view alias) const;

    Future<void> wait_ready();
    Future<void> wait_ready(std::chrono::milliseconds timeout);

    Future<void> send(std::string_view alias, const void* buffer, size_t length, Tag tag);
    Future<void> send(uint32_t rank, const void* buffer, size_t length, Tag tag);

    Future<std::pair<size_t, Tag>>
    recv_any(void* buffer, size_t length, Tag tag, Tag tag_mask = kTagMaskAll);

private:
    Cluster(Context::Ptr ctx, Worker::Ptr worker, bool is_master);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace axon
