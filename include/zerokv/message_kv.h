#pragma once

/// @file zerokv/message_kv.h
/// @brief Message-style wrapper API on top of ZeroKV KV primitives.

#include "zerokv/kv.h"
#include "zerokv/memory.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace zerokv {

class MessageKV {
public:
    using Ptr = std::shared_ptr<MessageKV>;

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

    ~MessageKV();
    MessageKV(const MessageKV&) = delete;
    MessageKV& operator=(const MessageKV&) = delete;

    void start(const zerokv::kv::NodeConfig& cfg);
    void stop();

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
    explicit MessageKV(const zerokv::Config& cfg);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zerokv
