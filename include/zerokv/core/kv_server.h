#pragma once

/// @file zerokv/core/kv_server.h
/// @brief KV metadata server infrastructure API.

#include "zerokv/config.h"
#include "zerokv/core/common_types.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zerokv::kv {

class KVServer {
public:
    using Ptr = std::shared_ptr<KVServer>;

    static Ptr create(const zerokv::Config& cfg = {});

    ~KVServer();
    KVServer(const KVServer&) = delete;
    KVServer& operator=(const KVServer&) = delete;

    zerokv::Status start(const ServerConfig& cfg);
    void stop();

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] std::string address() const;
    [[nodiscard]] std::optional<KeyInfo> lookup(const std::string& key) const;
    [[nodiscard]] std::vector<std::string> list_keys() const;

private:
    explicit KVServer(const zerokv::Config& cfg);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zerokv::kv

namespace zerokv {
namespace core {

using KVServer = ::zerokv::kv::KVServer;

}  // namespace core
}  // namespace zerokv
