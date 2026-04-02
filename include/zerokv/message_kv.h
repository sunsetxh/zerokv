#pragma once

/// @file zerokv/message_kv.h
/// @brief Message-style wrapper API on top of ZeroKV KV primitives.

#include "zerokv/core/kv_node.h"
#include "zerokv/core/kv_server.h"
#include "zerokv/memory.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace zerokv {

class KV {
public:
    using Ptr = std::shared_ptr<KV>;

    struct BatchRecvItem {
        std::string key;
        size_t length = 0;
        size_t offset = 0;
    };

    struct BatchRecvResult {
        std::vector<std::string> completed;
        std::vector<std::string> failed;
        std::vector<std::string> timed_out;
        bool completed_all = false;
    };

    static Ptr create(const zerokv::Config& cfg = {});

    ~KV();
    KV(const KV&) = delete;
    KV& operator=(const KV&) = delete;

    void start(const zerokv::kv::NodeConfig& cfg);
    void stop();

    [[nodiscard]] zerokv::MemoryRegion::Ptr allocate_send_region(size_t size);

    void send(const std::string& key, const void* data, size_t size);
    void send_region(const std::string& key,
                     const zerokv::MemoryRegion::Ptr& region,
                     size_t size);

    void recv(const std::string& key,
              const zerokv::MemoryRegion::Ptr& region,
              size_t length,
              size_t offset,
              std::chrono::milliseconds timeout);

    BatchRecvResult recv_batch(const std::vector<BatchRecvItem>& items,
                               const zerokv::MemoryRegion::Ptr& region,
                               std::chrono::milliseconds timeout);

private:
    explicit KV(const zerokv::Config& cfg);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

using MessageKV [[deprecated("use zerokv::KV")]] = KV;

}  // namespace zerokv
