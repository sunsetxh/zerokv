#pragma once

/// @file zerokv/endpoint.h
/// @brief Endpoint – a connection to a remote peer with send/recv and RDMA APIs.
///
/// An Endpoint is always associated with exactly one Worker.
/// All operations on an Endpoint must be called from the Worker's thread
/// (or while holding the Worker's progress lock).
///
/// Two-sided (tag-matched):
///   ep->tag_send(buf, len, tag);
///   ep->tag_recv(buf, len, tag);
///
/// One-sided RDMA:
///   ep->put(local_region, remote_addr, remote_key);
///   ep->get(local_region, remote_addr, remote_key);

#include "zerokv/common.h"
#include "zerokv/config.h"
#include "zerokv/future.h"
#include "zerokv/memory.h"
#include "zerokv/worker.h"

#include <memory>
#include <string>

namespace zerokv {

// Forward declarations
class Worker;

// ---------------------------------------------------------------------------
// Endpoint
// ---------------------------------------------------------------------------

class Endpoint : public std::enable_shared_from_this<Endpoint> {
public:
    using Ptr = std::shared_ptr<Endpoint>;

    ~Endpoint();
    Endpoint(const Endpoint&) = delete;
    Endpoint& operator=(const Endpoint&) = delete;

    // =========================================================================
    // Two-sided (tag-matched) messaging
    // =========================================================================

    /// --- Send ---------------------------------------------------------------

    /// Send from a raw buffer (will be internally registered or copied).
    /// For buffers < ~8 KB UCX uses eager (inline); larger uses rendezvous.
    Future<void> tag_send(const void* buffer, size_t length, Tag tag);

    /// Send from a pre-registered region (zero-copy for large messages).
    Future<void> tag_send(const MemoryRegion::Ptr& region, Tag tag);

    /// Send from a pre-registered region with offset+length sub-range.
    Future<void> tag_send(const MemoryRegion::Ptr& region,
                          size_t offset, size_t length, Tag tag);

    /// --- Recv ---------------------------------------------------------------

    /// Receive into a raw buffer.
    /// Returns (bytes_received, matched_tag).
    Future<std::pair<size_t, Tag>>
    tag_recv(void* buffer, size_t length, Tag tag, Tag tag_mask = kTagMaskAll);

    /// Receive into a pre-registered region.
    Future<std::pair<size_t, Tag>>
    tag_recv(const MemoryRegion::Ptr& region, Tag tag, Tag tag_mask = kTagMaskAll);

    // =========================================================================
    // One-sided RDMA (RMA)
    // =========================================================================

    /// --- Put (write to remote) ----------------------------------------------

    /// Write local data to remote memory.
    /// @param local_region  Pre-registered local buffer.
    /// @param local_offset  Offset within local region.
    /// @param remote_addr   Remote virtual address (base + offset on peer).
    /// @param rkey          Remote key obtained from the peer.
    /// @param length        Bytes to transfer.
    Future<void> put(const MemoryRegion::Ptr& local_region,
                     size_t                   local_offset,
                     uint64_t                 remote_addr,
                     const RemoteKey&         rkey,
                     size_t                   length);

    /// Convenience: put the entire local region.
    Future<void> put(const MemoryRegion::Ptr& local_region,
                     uint64_t                 remote_addr,
                     const RemoteKey&         rkey);

    /// --- Get (read from remote) ---------------------------------------------

    Future<void> get(const MemoryRegion::Ptr& local_region,
                     size_t                   local_offset,
                     uint64_t                 remote_addr,
                     const RemoteKey&         rkey,
                     size_t                   length);

    Future<void> get(const MemoryRegion::Ptr& local_region,
                     uint64_t                 remote_addr,
                     const RemoteKey&         rkey);

    /// --- Atomic RDMA operations ---------------------------------------------

    /// Remote atomic fetch-and-add.
    Future<uint64_t> atomic_fadd(const MemoryRegion::Ptr& local, size_t offset,
                                 uint64_t value, uint64_t remote_addr,
                                 const RemoteKey& rkey);

    /// Remote atomic fetch-and-add (simplified).
    Future<uint64_t> atomic_fadd(uint64_t     remote_addr,
                                 const RemoteKey& rkey,
                                 uint64_t     value);

    /// Remote atomic compare-and-swap.
    Future<uint64_t> atomic_cswap(uint64_t     remote_addr,
                                  const RemoteKey& rkey,
                                  uint64_t     expected,
                                  uint64_t     desired);

    /// --- Flush / fence ------------------------------------------------------

    /// Ensure all outstanding RMA operations to this endpoint are visible
    /// on the remote side.
    Future<void> flush();

    // =========================================================================
    // Stream (byte-oriented, ordered)
    // =========================================================================

    /// Send a contiguous stream of bytes (no tag).
    Future<void> stream_send(const void* buffer, size_t length);
    Future<void> stream_send(const MemoryRegion::Ptr& region);

    /// Receive up to @p length bytes from the stream.
    Future<size_t> stream_recv(void* buffer, size_t length);
    Future<size_t> stream_recv(const MemoryRegion::Ptr& region);

    // =========================================================================
    // Connection management
    // =========================================================================

    /// Graceful close (sends disconnect notification to peer).
    Future<void> close();

    /// Check if the endpoint is still connected.
    [[nodiscard]] bool is_connected() const noexcept;

    /// Remote peer address string.
    [[nodiscard]] std::string remote_address() const;

    /// Set error callback for asynchronous connection errors.
    void set_error_callback(std::function<void(Status)> cb);

    /// Native handle (ucp_ep_h).
    [[nodiscard]] void* native_handle() const noexcept;

    /// The worker this endpoint belongs to.
    [[nodiscard]] std::shared_ptr<Worker> worker() const noexcept;

    Endpoint() = default;

    /// Create endpoint from worker (used internally by Worker)
    static Ptr create(Worker::Ptr worker, void* handle);

    struct Impl;
    std::unique_ptr<Impl> impl_;

private:
    friend class Worker;
};

}  // namespace zerokv
