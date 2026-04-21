#pragma once

#include "zerokv/common.h"
#include "zerokv/config.h"
#include "zerokv/transport/future.h"
#include "zerokv/transport/endpoint.h"
#include "zerokv/transport/memory.h"
#include "zerokv/transport/worker.h"

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace zerokv::compat {

// AlpsKvChannel supports two roles:
//
//   Listen (RANK0 / server): one Worker accepts multiple incoming connections.
//     ReadBytes / ReadBytesBatch use worker-level tag matching, so messages
//     from any connected sender are matched by tag regardless of which endpoint
//     they arrived on.  ReadBytesBatch posts all recvs in parallel and waits
//     for all, enabling concurrent multi-sender scenarios.
//
//   Connect (RANK1 / client): the first WriteBytes call from each OS thread
//     transparently creates a dedicated Worker+Endpoint for that thread and
//     connects to the server.  Subsequent calls on the same thread reuse the
//     cached state.  Multiple threads therefore send truly in parallel without
//     any cross-thread locking on the hot path.
class AlpsKvChannel {
public:
    struct WriteTimingStats {
        uint64_t write_ops = 0;
        uint64_t control_request_grant_us = 0;
        uint64_t rdma_put_us = 0;
        uint64_t flush_us = 0;
        uint64_t write_done_us = 0;
    };

    struct ReceivePathStats {
        uint64_t direct_grant_ops = 0;
        uint64_t staged_grant_ops = 0;
        uint64_t staged_delivery_ops = 0;
        uint64_t staged_copy_bytes = 0;
        uint64_t staged_copy_us = 0;
    };

#ifdef ZEROKV_ALPS_TEST_HOOKS
    struct DebugStats {
        size_t payload_tag_send_ops = 0;
        size_t rma_put_ops = 0;
        size_t receive_slot_register_ops = 0;
        size_t remote_rkey_unpack_ops = 0;
    };
#endif

    AlpsKvChannel();
    ~AlpsKvChannel();

    AlpsKvChannel(const AlpsKvChannel&) = delete;
    AlpsKvChannel& operator=(const AlpsKvChannel&) = delete;

    bool Listen(const std::string& bind_address, int connect_timeout_ms);
    bool Connect(const std::string& remote_address, int connect_timeout_ms);
    void Shutdown();

    bool WriteBytes(const void* data, size_t size, int tag, int index, int src, int dst);
    void ReadBytes(void* data, size_t size, int tag, int index, int src, int dst);
    void ReadBytesBatch(std::vector<void*>& data,
                        const std::vector<size_t>& sizes,
                        const std::vector<int>& tags,
                        const std::vector<int>& indices,
                        const std::vector<int>& srcs,
                        const std::vector<int>& dsts);

    [[nodiscard]] std::string local_address() const;
    [[nodiscard]] WriteTimingStats write_timing_stats() const;
    void reset_write_timing_stats();
    [[nodiscard]] ReceivePathStats receive_path_stats() const;
    void reset_receive_path_stats();

#ifdef ZEROKV_ALPS_TEST_HOOKS
    [[nodiscard]] DebugStats debug_stats() const;
#endif

private:
    // ---- key helpers -------------------------------------------------------
    struct BufferKey {
        const void* data = nullptr;
        size_t size = 0;
        bool operator==(const BufferKey& o) const noexcept {
            return data == o.data && size == o.size;
        }
    };
    struct BufferKeyHash {
        size_t operator()(const BufferKey& k) const noexcept {
            return std::hash<const void*>{}(k.data) ^ (std::hash<size_t>{}(k.size) << 1U);
        }
    };
    using SendCache = std::unordered_map<BufferKey,
                                         zerokv::transport::MemoryRegion::Ptr,
                                         BufferKeyHash>;
    struct RegisteredReceiveBuffer {
        zerokv::transport::MemoryRegion::Ptr region;
        zerokv::transport::RemoteKey remote_key;
    };
    using ReceiveCache = std::unordered_map<BufferKey,
                                            std::shared_ptr<RegisteredReceiveBuffer>,
                                            BufferKeyHash>;

    static zerokv::Tag MakeMessageTag(int tag, int index, int src, int dst);
    zerokv::transport::MemoryRegion::Ptr GetOrRegisterSendRegion(
        SendCache& cache, const void* data, size_t size);
    static std::string ExtractHost(const std::string& address);
    static std::string MakeAddress(const std::string& host, uint16_t port);
    static uint16_t ExtractPort(const std::string& address);

    // ---- context -----------------------------------------------------------
    enum class Mode { kUninitialized, kListen, kConnect };

    bool InitContext();  // creates shared context_

    zerokv::Context::Ptr context_;
    Mode mode_ = Mode::kUninitialized;
    bool running_ = false;
    std::string local_address_;
    std::string control_address_;

    // ---- listen-mode state (RANK0) -----------------------------------------
    // One worker drives UCX progress for all accepted connections.
    // tag_recv is worker-level, so it matches incoming messages from any peer.
    zerokv::transport::Worker::Ptr recv_worker_;
    zerokv::transport::Listener::Ptr listener_;
    mutable std::mutex endpoints_mutex_;
    std::condition_variable endpoints_cv_;
    std::vector<zerokv::transport::Endpoint::Ptr> endpoints_;
    struct PendingBootstrapSend {
        zerokv::transport::Future<void> future;
        zerokv::transport::MemoryRegion::Ptr region;
    };
    std::vector<PendingBootstrapSend> pending_bootstrap_sends_;

    zerokv::transport::Endpoint::Ptr WaitForAnyEndpoint(std::chrono::milliseconds timeout);
    bool InitControlListener(const std::string& bind_address);
    void QueueBootstrapControlPort(const zerokv::transport::Endpoint::Ptr& ep);
    void ReapBootstrapSends();
    void ControlAcceptLoop();
    void ControlConnectionLoop(int fd);

    struct ReceiveSlot {
        zerokv::transport::MemoryRegion::Ptr region;
        zerokv::transport::RemoteKey remote_key;
        size_t size = 0;
        uint64_t reservation_id = 0;
        bool reserved = false;
        bool done = false;
        bool success = false;
        std::string error;
        std::mutex mutex;
        std::condition_variable cv;
    };
    mutable std::mutex receive_slots_mutex_;
    std::condition_variable receive_slots_cv_;
    std::unordered_map<zerokv::Tag, std::shared_ptr<ReceiveSlot>> receive_slots_;
    ReceiveCache receive_cache_;
    struct BufferedMessage {
        zerokv::transport::MemoryRegion::Ptr region;
        zerokv::transport::RemoteKey remote_key;
        size_t size = 0;
        uint64_t reservation_id = 0;
        bool completed = false;
    };
    std::unordered_map<zerokv::Tag, std::shared_ptr<BufferedMessage>> staged_messages_;
    std::atomic<uint64_t> next_reservation_id_{1};
    int control_listen_fd_ = -1;
    std::thread control_accept_thread_;
    mutable std::mutex control_threads_mutex_;
    std::vector<std::thread> control_threads_;
    mutable std::mutex active_control_fds_mutex_;
    std::unordered_set<int> active_control_fds_;

    std::shared_ptr<ReceiveSlot> RegisterReceiveSlot(void* data, size_t size, zerokv::Tag message_tag);
    void FinishReceiveSlot(const std::shared_ptr<ReceiveSlot>& slot, const std::string& error);
    void RemoveReceiveSlot(zerokv::Tag message_tag, const std::shared_ptr<ReceiveSlot>& slot);
    bool WaitForSlotCompletion(const std::shared_ptr<ReceiveSlot>& slot);
    void TryDeliverBufferedMessage(zerokv::Tag message_tag);

    // ---- connect-mode state (RANK1) ----------------------------------------
    // Each OS thread gets its own Worker+Endpoint so sends are fully parallel.
    std::string remote_address_;
    int connect_timeout_ms_ = 5000;

    struct PerThreadState {
        zerokv::transport::Worker::Ptr worker;
        zerokv::transport::Endpoint::Ptr endpoint;
        SendCache send_cache;
        std::unordered_set<std::string> remote_rkey_cache;
        std::string control_address;
        int control_fd = -1;
        uint64_t next_request_id = 1;
    };
    mutable std::mutex per_thread_mutex_;
    std::map<std::thread::id, std::shared_ptr<PerThreadState>> per_thread_states_;

    PerThreadState* GetOrCreateThreadState();
    bool BootstrapControlAddress(PerThreadState* state,
                                 std::chrono::steady_clock::time_point deadline);
    bool EnsureControlConnection(PerThreadState* state);
    static void CloseControlFd(int* fd);

    std::atomic<uint64_t> write_ops_{0};
    std::atomic<uint64_t> control_request_grant_us_{0};
    std::atomic<uint64_t> rdma_put_us_{0};
    std::atomic<uint64_t> flush_us_{0};
    std::atomic<uint64_t> write_done_us_{0};
    std::atomic<uint64_t> direct_grant_ops_{0};
    std::atomic<uint64_t> staged_grant_ops_{0};
    std::atomic<uint64_t> staged_delivery_ops_{0};
    std::atomic<uint64_t> staged_copy_bytes_{0};
    std::atomic<uint64_t> staged_copy_us_{0};

#ifdef ZEROKV_ALPS_TEST_HOOKS
    std::atomic<size_t> payload_tag_send_ops_{0};
    std::atomic<size_t> rma_put_ops_{0};
    std::atomic<size_t> receive_slot_register_ops_{0};
    std::atomic<size_t> remote_rkey_unpack_ops_{0};
#endif
};

}  // namespace zerokv::compat
