#include "zerokv/transport/future.h"

#include "zerokv/common.h"
#include "zerokv/transport/endpoint.h"

#include <ucp/api/ucp.h>

#include <optional>
#include <functional>
#include <vector>

namespace zerokv::transport {

// ============================================================================
// Request
// ============================================================================

Request::Ptr Request::create(void* ucx_request, ucp_worker_h worker, size_t initial_bytes) {
    if (!ucx_request) {
        return nullptr;
    }
    auto req = Ptr(new Request());
    req->impl_ = std::make_unique<Impl>();
    req->impl_->ucx_request_ = ucx_request;
    req->impl_->worker_ = worker;
    req->impl_->bytes_transferred_ = initial_bytes;
    return req;
}

Request::Ptr Request::create(void* ucx_request, ucp_worker_h worker, size_t initial_bytes,
                              std::shared_ptr<void> keep_alive) {
    if (!ucx_request) {
        return nullptr;
    }
    auto req = Ptr(new Request());
    req->impl_ = std::make_unique<Impl>();
    req->impl_->ucx_request_ = ucx_request;
    req->impl_->worker_ = worker;
    req->impl_->bytes_transferred_ = initial_bytes;
    req->impl_->keep_alive_ = std::move(keep_alive);
    return req;
}

Request::Ptr Request::create(void* ucx_request, ucp_worker_h worker,
                             std::shared_ptr<size_t> async_bytes_transferred,
                             std::shared_ptr<void> keep_alive) {
    if (!ucx_request) {
        return nullptr;
    }
    auto req = Ptr(new Request());
    req->impl_ = std::make_unique<Impl>();
    req->impl_->ucx_request_ = ucx_request;
    req->impl_->worker_ = worker;
    req->impl_->async_bytes_transferred_ = std::move(async_bytes_transferred);
    req->impl_->keep_alive_ = std::move(keep_alive);
    return req;
}

Request::Ptr Request::create(void* ucx_request, ucp_worker_h worker,
                             std::shared_ptr<size_t> async_bytes_transferred,
                             std::shared_ptr<Tag> async_matched_tag,
                             std::shared_ptr<void> keep_alive) {
    if (!ucx_request) {
        return nullptr;
    }
    auto req = Ptr(new Request());
    req->impl_ = std::make_unique<Impl>();
    req->impl_->ucx_request_ = ucx_request;
    req->impl_->worker_ = worker;
    req->impl_->async_bytes_transferred_ = std::move(async_bytes_transferred);
    req->impl_->async_matched_tag_ = std::move(async_matched_tag);
    req->impl_->keep_alive_ = std::move(keep_alive);
    return req;
}

Request::~Request() {
    if (impl_ && impl_->ucx_request_) {
        // Cancel the request if still in progress
        if (ucp_request_check_status(impl_->ucx_request_) == UCS_INPROGRESS) {
            if (impl_->worker_) {
                ucp_request_cancel(impl_->worker_, impl_->ucx_request_);
                // Progress until cancel completes
                for (int i = 0; i < 1000 && ucp_request_check_status(impl_->ucx_request_) == UCS_INPROGRESS; ++i) {
                    ucp_worker_progress(impl_->worker_);
                }
            }
        }
        ucp_request_free(impl_->ucx_request_);
    }
}

Request::Request(Request&&) noexcept = default;
Request& Request::operator=(Request&&) noexcept = default;

bool Request::is_complete() const noexcept {
    if (!impl_ || !impl_->ucx_request_) {
        return true;
    }
    return ucp_request_check_status(impl_->ucx_request_) != UCS_INPROGRESS;
}

Status Request::status() const noexcept {
    if (!impl_) {
        return Status::OK();
    }
    if (!impl_->ucx_request_) {
        return impl_->status_;
    }
    ucs_status_t ucs_status = ucp_request_check_status(impl_->ucx_request_);
    if (ucs_status == UCS_INPROGRESS) {
        return Status(ErrorCode::kInProgress);
    }
    if (ucs_status != UCS_OK) {
        return Status(ErrorCode::kTransportError, ucs_status_string(ucs_status));
    }
    return Status::OK();
}

Status Request::wait(std::chrono::milliseconds timeout) {
    if (!impl_ || !impl_->ucx_request_) {
        return Status::OK();
    }

    if (timeout.count() < 0) {
        while (ucp_request_check_status(impl_->ucx_request_) == UCS_INPROGRESS) {
            ucp_worker_progress(impl_->worker_);
        }
    } else {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (ucp_request_check_status(impl_->ucx_request_) == UCS_INPROGRESS) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return Status(ErrorCode::kTimeout);
            }
            ucp_worker_progress(impl_->worker_);
        }
    }

    ucs_status_t ucs_status = ucp_request_check_status(impl_->ucx_request_);
    if (ucs_status != UCS_OK) {
        return Status(ErrorCode::kTransportError, ucs_status_string(ucs_status));
    }

    return Status::OK();
}

size_t Request::bytes_transferred() const noexcept {
    if (!impl_ || !impl_->ucx_request_) {
        return 0;
    }
    if (impl_->async_bytes_transferred_) {
        return *impl_->async_bytes_transferred_;
    }
    return impl_->bytes_transferred_;
}

Tag Request::matched_tag() const noexcept {
    if (!impl_ || !impl_->ucx_request_) {
        return 0;
    }
    if (impl_->async_matched_tag_) {
        return *impl_->async_matched_tag_;
    }
    return impl_->matched_tag_;
}

void* Request::native_handle() const noexcept {
    return impl_ ? impl_->ucx_request_ : nullptr;
}

uint64_t Request::atomic_result() const noexcept {
    if (!impl_ || !impl_->is_atomic_ || !impl_->async_bytes_transferred_) {
        return 0;
    }
    return static_cast<uint64_t>(*impl_->async_bytes_transferred_);
}

void Request::cancel() {
}

void Request::populate_recv_info() const {
    // Completion metadata is captured via per-operation callbacks.
}

} // namespace zerokv::transport
