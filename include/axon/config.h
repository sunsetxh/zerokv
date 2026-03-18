#pragma once

/// @file axon/config.h
/// @brief Configuration and context initialization for the AXON library.
///
/// Usage:
///   auto cfg = axon::Config::builder()
///                  .set_transport("ucx")
///                  .set_num_workers(4)
///                  .set_memory_pool_size(256 * 1024 * 1024)
///                  .build();
///   auto ctx = axon::Context::create(cfg);

#include "axon/common.h"

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>

namespace axon {

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

class Config {
public:
    class Builder;

    /// Default-constructed config uses environment-driven defaults.
    Config();
    ~Config();
    Config(const Config&);
    Config& operator=(const Config&);
    Config(Config&&) noexcept;
    Config& operator=(Config&&) noexcept;
    Config& copy_from(const Config& other);

    // --- Accessors -----------------------------------------------------------

    /// Transport backend name: "ucx" (default), "tcp", "shm".
    [[nodiscard]] const std::string& transport() const noexcept;

    /// Number of UCX workers (progress threads).  0 = auto (one per NUMA node).
    [[nodiscard]] size_t num_workers() const noexcept;

    /// Pre-allocated memory-pool size in bytes (default 64 MiB).
    [[nodiscard]] size_t memory_pool_size() const noexcept;

    /// Maximum in-flight requests per worker (back-pressure threshold).
    [[nodiscard]] size_t max_inflight_requests() const noexcept;

    /// Timeout for connection establishment.
    [[nodiscard]] std::chrono::milliseconds connect_timeout() const noexcept;

    /// Enable registration cache for memory regions.
    [[nodiscard]] bool registration_cache_enabled() const noexcept;

    /// Maximum registration cache entries (0 = unlimited).
    [[nodiscard]] size_t registration_cache_max_entries() const noexcept;

    /// Generic key-value store for transport-specific options.
    [[nodiscard]] std::string get(const std::string& key,
                                  const std::string& default_val = {}) const;

    // --- Builder factory -----------------------------------------------------

    static Builder builder();

private:
    friend class Context;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Config::Builder  (fluent API)
// ---------------------------------------------------------------------------

class Config::Builder {
public:
    Builder();
    ~Builder();

    Builder& set_transport(std::string name);
    Builder& set_num_workers(size_t n);
    Builder& set_memory_pool_size(size_t bytes);
    Builder& set_max_inflight_requests(size_t n);
    Builder& set_connect_timeout(std::chrono::milliseconds ms);
    Builder& enable_registration_cache(bool enable = true);
    Builder& set_registration_cache_max_entries(size_t n);

    /// Set an arbitrary key-value option forwarded to the transport backend.
    /// Examples: ("UCX_TLS", "rc,ud"), ("UCX_NET_DEVICES", "mlx5_0:1").
    Builder& set(std::string key, std::string value);

    /// Populate from environment variables prefixed with AXON_.
    Builder& from_env();

    Config build();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Context – top-level library handle
// ---------------------------------------------------------------------------

/// A Context owns the UCX context (ucp_context_h), memory pools, and worker
/// threads.  It is thread-safe; multiple threads may share the same Context.
///
/// Lifetime: Context must outlive all Workers, Endpoints, and MemoryRegions
/// created from it.
class Context : public std::enable_shared_from_this<Context> {
public:
    using Ptr = std::shared_ptr<Context>;

    /// Create a context from configuration.
    static Ptr create(const Config& config = {});

    ~Context();

    // non-copyable, non-movable (shared_ptr semantics)
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    /// Access the active configuration (read-only after creation).
    [[nodiscard]] const Config& config() const noexcept;

    /// Query supported memory types on this system.
    [[nodiscard]] bool supports_memory_type(MemoryType mt) const noexcept;

    /// Query whether RDMA (one-sided put/get) is available.
    [[nodiscard]] bool supports_rma() const noexcept;

    /// Query whether tag matching is available in hardware.
    [[nodiscard]] bool supports_hw_tag_matching() const noexcept;

    /// Opaque handle for FFI / plugin access.
    [[nodiscard]] void* native_handle() const noexcept;

private:
    explicit Context(const Config& config);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace axon
