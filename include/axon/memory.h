#pragma once

/// @file axon/memory.h
/// @brief Memory registration, remote keys, and memory pool interfaces.
///
/// Key design points:
///   - MemoryRegion wraps UCX ucp_mem_h + rkey for one-sided RDMA.
///   - RegistrationCache amortises registration cost across repeated sends.
///   - MemoryPool provides slab-based allocation of registered buffers.
///
/// Usage:
///   auto region = axon::MemoryRegion::register_mem(ctx, ptr, len, MemoryType::kHost);
///   auto pool   = axon::MemoryPool::create(ctx, 64 * 1024 * 1024);
///   auto buf    = pool->allocate(4096);

#include "axon/common.h"
#include "axon/config.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace axon {

// ---------------------------------------------------------------------------
// MemoryRegion – registered memory for zero-copy RDMA
// ---------------------------------------------------------------------------

/// Packed remote-key blob that can be sent to a peer for one-sided access.
struct RemoteKey {
    std::vector<uint8_t> data;   // serialised rkey

    [[nodiscard]] bool empty() const noexcept { return data.empty(); }
    [[nodiscard]] size_t size() const noexcept { return data.size(); }
    [[nodiscard]] const uint8_t* bytes() const noexcept { return data.data(); }
};

/// A MemoryRegion represents a contiguous block of memory that has been
/// registered (pinned) with the transport layer.  For UCX this wraps
/// ucp_mem_h and enables zero-copy send/recv and RDMA put/get.
///
/// Thread-safety: a MemoryRegion can be read concurrently; mutation (resize,
/// deregister) requires external synchronisation.
class MemoryRegion {
public:
    using Ptr = std::shared_ptr<MemoryRegion>;

    /// Register an existing user buffer.
    /// @param ctx    Owning context.
    /// @param addr   Start address (may be device pointer for GPU memory).
    /// @param length Size in bytes.
    /// @param type   Memory type (host, cuda, rocm, ascend).
    static Ptr register_mem(const Context::Ptr& ctx,
                            void*               addr,
                            size_t              length,
                            MemoryType          type = MemoryType::kHost);

    /// Allocate *and* register a buffer in one step.
    /// The library manages both allocation and registration lifetime.
    static Ptr allocate(const Context::Ptr& ctx,
                        size_t              length,
                        MemoryType          type = MemoryType::kHost);

    ~MemoryRegion();
    MemoryRegion(const MemoryRegion&) = delete;
    MemoryRegion& operator=(const MemoryRegion&) = delete;

    [[nodiscard]] void*       address() const noexcept;
    [[nodiscard]] size_t      length() const noexcept;
    [[nodiscard]] MemoryType  memory_type() const noexcept;

    /// Get the remote key for this region (needed by the peer for put/get).
    [[nodiscard]] RemoteKey   remote_key() const;

    /// Typed view (host memory only – asserts kHost).
    template <typename T>
    [[nodiscard]] std::span<T> as_span() const {
        return {static_cast<T*>(address()), length() / sizeof(T)};
    }

    /// Opaque native handle (ucp_mem_h).
    [[nodiscard]] void* native_handle() const noexcept;

private:
    MemoryRegion() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// MemoryPool – slab allocator over registered memory
// ---------------------------------------------------------------------------

/// A MemoryPool pre-allocates and pre-registers a large region, then hands
/// out fixed-size or variable-size slabs.  This eliminates per-transfer
/// registration overhead for the hot path.
///
/// Thread-safety: all public methods are thread-safe (internal locking).
class MemoryPool {
public:
    using Ptr = std::shared_ptr<MemoryPool>;

    /// @param ctx        Owning context.
    /// @param pool_bytes Total pool size.
    /// @param type       Memory type.
    static Ptr create(const Context::Ptr& ctx,
                      size_t              pool_bytes,
                      MemoryType          type = MemoryType::kHost);

    ~MemoryPool();

    /// Allocate a buffer of at least @p size bytes from the pool.
    /// Returns nullptr if the pool is exhausted (never throws).
    struct Buffer {
        void*  data   = nullptr;
        size_t length = 0;
        /// The underlying registered region (keeps registration alive).
        MemoryRegion::Ptr region;

        [[nodiscard]] bool valid() const noexcept { return data != nullptr; }
        explicit operator bool() const noexcept { return valid(); }
    };

    [[nodiscard]] Buffer allocate(size_t size);

    /// Return a buffer to the pool.
    void deallocate(Buffer& buf);

    // --- Stats ---------------------------------------------------------------

    [[nodiscard]] size_t total_bytes() const noexcept;
    [[nodiscard]] size_t used_bytes() const noexcept;
    [[nodiscard]] size_t free_bytes() const noexcept;
    [[nodiscard]] size_t allocation_count() const noexcept;

private:
    MemoryPool() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// RegistrationCache – amortise repeated registrations
// ---------------------------------------------------------------------------

/// Caches ucp_mem_h registrations keyed by (address, length, memory_type).
/// When the user passes an unregistered buffer to send/put, the transport
/// layer looks up the cache first; on miss it registers and inserts.
///
/// Eviction: LRU with configurable max entries.
/// Thread-safety: all methods are thread-safe.
class RegistrationCache {
public:
    using Ptr = std::shared_ptr<RegistrationCache>;

    static Ptr create(const Context::Ptr& ctx, size_t max_entries = 0);

    ~RegistrationCache();

    /// Look up or register a memory region.
    [[nodiscard]] MemoryRegion::Ptr get_or_register(void*      addr,
                                                     size_t     length,
                                                     MemoryType type);

    /// Explicitly invalidate an entry (e.g. before freeing the buffer).
    void invalidate(void* addr, size_t length);

    /// Flush the entire cache.
    void flush();

    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] size_t hits() const noexcept;
    [[nodiscard]] size_t misses() const noexcept;

private:
    RegistrationCache() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace axon
