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

Request::Ptr Request::create(void* ucx_request, ucp_worker_h worker) {
    if (!ucx_request) {
        return nullptr;
    }
    auto req = Ptr(new Request());
    req->impl_ = std::make_unique<Impl>();
    req->impl_->ucx_request_ = ucx_request;
    req->impl_->worker_ = worker;
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
    bool complete = ucp_request_check_status(impl_->ucx_request_) != UCS_INPROGRESS;
    if (complete) {
        populate_recv_info();
    }
    return complete;
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

    // Extract receive info for tag receive operations
    populate_recv_info();
    return Status::OK();
}

size_t Request::bytes_transferred() const noexcept {
    if (!impl_ || !impl_->ucx_request_) {
        return 0;
    }
    return impl_->bytes_transferred_;
}

Tag Request::matched_tag() const noexcept {
    if (!impl_ || !impl_->ucx_request_) {
        return 0;
    }
    return impl_->matched_tag_;
}

void* Request::native_handle() const noexcept {
    return impl_ ? impl_->ucx_request_ : nullptr;
}

void Request::cancel() {
}

void Request::populate_recv_info() const {
    if (!impl_ || !impl_->ucx_request_ || impl_->recv_info_populated_) {
        return;
    }

    // Check if request is complete
    ucs_status_t status = ucp_request_check_status(impl_->ucx_request_);
    if (status == UCS_INPROGRESS) {
        return;  // Not complete yet
    }

    // Try to get tag receive info using ucp_tag_recv_request_test
    // This function only works for tag receive requests
    ucp_tag_recv_info_t recv_info;
    ucs_status_ptr_t req_ptr = impl_->ucx_request_;

    // ucp_tag_recv_request_test returns the request status and fills recv_info
    // Note: This is a UCX internal detail - the request data structure contains
    // the receive info after completion
    ucs_status_t recv_status = ucp_tag_recv_request_test(req_ptr, &recv_info);

    if (recv_status != UCS_INPROGRESS && recv_status != UCS_ERR_NOT_IMPLEMENTED) {
        // Successfully got receive info
        impl_->bytes_transferred_ = recv_info.length;
        impl_->matched_tag_ = recv_info.sender_tag;
        impl_->recv_info_populated_ = true;
    }
}

} // namespace p2p
