#pragma once

#include <ucp/api/ucp.h>

#include <memory>
#include <string>

namespace zerokv {
namespace internal {

/// RAII wrapper for UCX context
class UcxContext {
public:
    UcxContext();
    ~UcxContext();

    // Non-copyable
    UcxContext(const UcxContext&) = delete;
    UcxContext& operator=(const UcxContext&) = delete;

    // Movable
    UcxContext(UcxContext&& other) noexcept;
    UcxContext& operator=(UcxContext&& other) noexcept;

    /// Get underlying UCX context handle
    [[nodiscard]] ucp_context_h handle() const noexcept { return handle_; }

    /// Check if context is valid
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

    /// Get worker address (for connection exchange)
    [[nodiscard]] std::vector<uint8_t> worker_address() const;

private:
    ucp_context_h handle_;
};

} // namespace internal
} // namespace zerokv
