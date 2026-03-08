#pragma once

/// @file p2p/worker.h
/// @brief Worker – the UCX progress engine and endpoint factory.
///
/// Each Worker owns one ucp_worker_h.  A Worker is **single-threaded**: only
/// one thread may call progress()/poll() or initiate operations at a time.
/// For multi-threaded use, create one Worker per thread (or per NUMA node)
/// and partition endpoints across workers.
///
/// Usage:
///   auto worker = p2p::Worker::create(ctx);
///   auto ep     = worker->connect("192.168.1.2:13337");

#include "p2p/common.h"
#include "p2p/config.h"
#include "p2p/future.h"
#include "p2p/memory.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace p2p {

// Forward
class Context;
class Endpoint;

// ---------------------------------------------------------------------------
// Listener – server-side accept loop
// ---------------------------------------------------------------------------

/// Callback invoked when a new connection is accepted.
using AcceptCallback = std::function<void(std::shared_ptr<Endpoint> ep)>;

class Listener {
public:
    using Ptr = std::shared_ptr<Listener>;
    using WeakPtr = std::weak_ptr<Listener>;

    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    /// The address this listener is bound to (e.g. "0.0.0.0:13337").
    [[nodiscard]] std::string address() const;

    /// Add a pending connection request (called by connection handler).
    void add_connection_request(ucp_conn_request_h req);

    /// Accept one pending connection (call in event loop).
    bool accept();

    /// Stop accepting new connections.
    void close();

    /// Native handle (ucp_listener_h).
    [[nodiscard]] void* native_handle() const noexcept;

    /// Context this listener belongs to.
    [[nodiscard]] Context::Ptr context() const noexcept;

    Listener() = default;

    struct Impl;
    std::unique_ptr<Impl> impl_;

private:
    friend class Worker;
};

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

class Worker : public std::enable_shared_from_this<Worker> {
public:
    using Ptr = std::shared_ptr<Worker>;

    /// Create a worker bound to the given context.
    /// @param ctx      Owning context.
    /// @param index    Worker index (used for CPU affinity / NUMA binding).
    static Ptr create(const Context::Ptr& ctx, size_t index = 0);

    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    // --- Connection management -----------------------------------------------

    /// Actively connect to a remote peer using a pre-exchanged worker address.
    /// @param remote_address  The remote peer's worker address blob (from peer->address()).
    /// @return A future that resolves to the connected Endpoint.
    Future<std::shared_ptr<Endpoint>> connect(const std::vector<uint8_t>& remote_address);

    /// Actively connect to a remote peer (requires bootstrap mechanism).
    /// @param address  "host:port" - uses bootstrap if available, else returns error.
    /// @return A future that resolves to the connected Endpoint.
    Future<std::shared_ptr<Endpoint>> connect(const std::string& address);

    /// Start listening for incoming connections.
    /// @param bind_address  "host:port" to bind (":0" for OS-assigned port).
    /// @param on_accept     Callback invoked for each new connection.
    Listener::Ptr listen(const std::string& bind_address,
                         AcceptCallback      on_accept);

    // --- Progress (event loop) -----------------------------------------------

    /// Drive the UCX progress engine once (non-blocking).
    /// Returns true if any progress was made.
    bool progress() noexcept;

    /// Block until at least one event occurs or timeout expires.
    /// Internally uses ucp_worker_wait / epoll.
    bool wait(std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});

    /// Run the progress loop until @p pred returns true.
    template <typename Pred>
    void run_until(Pred pred) {
        while (!pred()) {
            if (!progress()) {
                wait(std::chrono::milliseconds{1});
            }
        }
    }

    /// Run the progress loop forever (blocking).
    /// Typically called from a dedicated thread.
    void run();

    /// Signal the worker to stop its run() loop.
    void stop() noexcept;

    // --- Worker-level tag recv -----------------------------------------------

    /// Post a receive that matches any endpoint.
    /// Useful for "any-source" receives.
    Future<std::pair<size_t, Tag>>
    tag_recv(void* buffer, size_t length, Tag tag, Tag tag_mask = kTagMaskAll);

    Future<std::pair<size_t, Tag>>
    tag_recv(const MemoryRegion::Ptr& region, Tag tag, Tag tag_mask = kTagMaskAll);

    // --- Query ---------------------------------------------------------------

    [[nodiscard]] Context::Ptr context() const noexcept;
    [[nodiscard]] size_t index() const noexcept;

    /// Obtain the worker address blob (for out-of-band exchange).
    [[nodiscard]] std::vector<uint8_t> address() const;

    /// Internal: obtain the raw worker address.
    [[nodiscard]] std::vector<uint8_t> worker_address() const;

    /// Native handle (ucp_worker_h).
    [[nodiscard]] void* native_handle() const noexcept;

    /// Get the file descriptor for epoll integration (Linux only, -1 if N/A).
    [[nodiscard]] int event_fd() const noexcept;

private:
    explicit Worker(const Context::Ptr& ctx, size_t index);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class Endpoint;
};

}  // namespace p2p
