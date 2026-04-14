#pragma once

#include "zerokv/common.h"
#include "zerokv/config.h"
#include "zerokv/transport/endpoint.h"
#include "zerokv/transport/memory.h"
#include "zerokv/transport/worker.h"

#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

    static zerokv::Tag MakeMessageTag(int tag, int index, int src, int dst);
    zerokv::transport::MemoryRegion::Ptr GetOrRegisterSendRegion(
        SendCache& cache, const void* data, size_t size);

    // ---- context -----------------------------------------------------------
    enum class Mode { kUninitialized, kListen, kConnect };

    bool InitContext();  // creates shared context_

    zerokv::Context::Ptr context_;
    Mode mode_ = Mode::kUninitialized;
    bool running_ = false;
    std::string local_address_;

    // ---- listen-mode state (RANK0) -----------------------------------------
    // One worker drives UCX progress for all accepted connections.
    // tag_recv is worker-level, so it matches incoming messages from any peer.
    zerokv::transport::Worker::Ptr recv_worker_;
    zerokv::transport::Listener::Ptr listener_;
    mutable std::mutex endpoints_mutex_;
    std::condition_variable endpoints_cv_;
    std::vector<zerokv::transport::Endpoint::Ptr> endpoints_;

    zerokv::transport::Endpoint::Ptr WaitForAnyEndpoint(std::chrono::milliseconds timeout);

    // ---- connect-mode state (RANK1) ----------------------------------------
    // Each OS thread gets its own Worker+Endpoint so sends are fully parallel.
    std::string remote_address_;
    int connect_timeout_ms_ = 5000;

    struct PerThreadState {
        zerokv::transport::Worker::Ptr worker;
        zerokv::transport::Endpoint::Ptr endpoint;
        SendCache send_cache;
    };
    mutable std::mutex per_thread_mutex_;
    std::map<std::thread::id, std::shared_ptr<PerThreadState>> per_thread_states_;

    PerThreadState* GetOrCreateThreadState();
};

}  // namespace zerokv::compat
