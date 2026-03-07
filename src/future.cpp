#include "p2p/future.h"

#include "p2p/common.h"
#include "p2p/endpoint.h"

#include <ucp/api/ucp.h>

#include <optional>
#include <functional>
#include <vector>

namespace p2p {

// ============================================================================
// Request
// ============================================================================

Request::~Request() = default;

bool Request::is_complete() const noexcept {
    return true;  // Stub
}

Status Request::status() const noexcept {
    return Status::OK();
}

Status Request::wait(std::chrono::milliseconds /*timeout*/) {
    return Status::OK();
}

size_t Request::bytes_transferred() const noexcept {
    return 0;
}

Tag Request::matched_tag() const noexcept {
    return 0;
}

void* Request::native_handle() const noexcept {
    return nullptr;
}

void Request::cancel() {
    // Stub
}

} // namespace p2p
