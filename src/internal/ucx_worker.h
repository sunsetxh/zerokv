#pragma once

#include <ucp/api/ucp.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <functional>

namespace p2p {
namespace internal {

/// RAII wrapper for UCX worker
class UcxWorker {
public:
    explicit UcxWorker(ucp_context_h context);
    ~UcxWorker();

    // Non-copyable
    UcxWorker(const UcxWorker&) = delete;
    UcxWorker& operator=(const UcxWorker&) = delete;

    // Movable
    UcxWorker(UcxWorker&& other) noexcept;
    UcxWorker& operator=(UcxWorker&& other) noexcept;

    /// Get underlying UCX worker handle
    [[nodiscard]] ucp_worker_h handle() const noexcept { return handle_; }

    /// Check if worker is valid
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

    /// Progress engine: drive completions
    /// @return true if any work was done
    bool progress() noexcept;

    /// Blocking wait with timeout
    /// @param timeout_ms timeout in milliseconds
    /// @return true if ready, false if timeout
    bool wait(uint64_t timeout_ms);

    /// Get event file descriptor for epoll integration
    [[nodiscard]] int event_fd() const noexcept { return event_fd_; }

    /// Get worker address for connection exchange
    [[nodiscard]] std::vector<uint8_t> address() const;

    /// Create endpoint to remote address
    ucp_ep_h create_endpoint(const uint8_t* address_data, size_t address_length);

    /// Create listener on given address
    ucp_listener_h create_listener(const char* address);

    /// Arm worker for async events
    ucs_status_t arm();

private:
    ucp_worker_h handle_;
    int event_fd_;
    std::atomic<bool> running_{true};
};

} // namespace internal
} // namespace p2p
