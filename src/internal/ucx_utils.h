#pragma once

#include <ucp/api/ucp.h>

#include <string>
#include <chrono>

namespace axon {
namespace internal {

/// Convert UCX status to string
[[nodiscard]] const char* status_string(ucs_status_t status) noexcept;

/// Check if status is success
[[nodiscard]] bool is_ok(ucs_status_t status) noexcept;

/// Check if status indicates in-progress (async operation pending)
[[nodiscard]] bool is_in_progress(ucs_status_t status) noexcept;

/// Convert UCX status to error code
int to_error_code(ucs_status_t status) noexcept;

/// Create UCX params for context
ucp_params_t create_context_params();

/// Create UCX params for worker
ucp_worker_params_t create_worker_params();

/// Parse address string (tcp://host:port, rdma://, etc.)
struct ParsedAddress {
    std::string protocol;
    std::string host;
    uint16_t port;
};

[[nodiscard]] ParsedAddress parse_address(const std::string& address);

/// Format timeout for UCX
[[nodiscard]] uint64_t to_ucx_timeout(std::chrono::milliseconds ms) noexcept;

} // namespace internal
} // namespace axon
