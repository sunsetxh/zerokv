#include "p2p/endpoint.h"

#include "p2p/worker.h"
#include "p2p/memory.h"
#include "p2p/future.h"
#include "p2p/common.h"

#include <ucp/api/ucp.h>

namespace p2p {

// ============================================================================
// Endpoint::Impl
// ============================================================================

struct Endpoint::Impl {
    Worker::Ptr worker_;
    ucp_ep_h handle_ = nullptr;

    ~Impl() {
        if (handle_) {
            // Note: Should be closed via close_nbx before destruction
        }
    }
};

// ============================================================================
// Endpoint
// ============================================================================

Endpoint::~Endpoint() = default;

Endpoint::Ptr Endpoint::create(Worker::Ptr worker, void* handle) {
    auto ep = Ptr(new Endpoint());
    ep->impl_ = std::make_unique<Endpoint::Impl>();
    ep->impl_->worker_ = std::move(worker);
    ep->impl_->handle_ = static_cast<ucp_ep_h>(handle);
    return ep;
}

// Tag messaging
Future<void> Endpoint::tag_send(const void* buffer, size_t length, Tag tag) {
    if (!impl_ || !impl_->handle_ || !buffer || length == 0) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument, "Invalid parameters"));
    }

    ucp_request_param_t params = {};
    params.op_attr_mask = UCP_OP_ATTR_FLAG_NO_IMM_CMPL;

    ucs_status_ptr_t status = ucp_tag_send_nbx(
        impl_->handle_,
        buffer,
        length,
        tag,
        &params);

    if (status == nullptr) {
        // Immediate completion
        return Future<void>::make_ready();
    }

    if (UCS_PTR_IS_ERR(status)) {
        ucs_status_t err = UCS_PTR_STATUS(status);
        return Future<void>::make_error(
            Status(ErrorCode::kTransportError, std::string("Send failed: ") + ucs_status_string(err)));
    }

    // Async - for now return ready (would need request tracking)
    return Future<void>::make_ready();
}

Future<void> Endpoint::tag_send(const MemoryRegion::Ptr& /*region*/, size_t /*offset*/,
                                size_t /*length*/, Tag /*tag*/) {
    // TODO: Implement with pre-registered memory region
    return Future<void>::make_error(
        Status(ErrorCode::kNotImplemented, "tag_send from region not implemented"));
}

Future<std::pair<size_t, Tag>> Endpoint::tag_recv(void* buffer, size_t length,
                                                 Tag tag, Tag tag_mask) {
    if (!impl_ || !impl_->worker_ || !buffer || length == 0) {
        return Future<std::pair<size_t, Tag>>::make_error(
            Status(ErrorCode::kInvalidArgument, "Invalid parameters"));
    }

    ucp_request_param_t params = {};
    params.op_attr_mask = UCP_OP_ATTR_FLAG_NO_IMM_CMPL;

    // Use worker handle for tag recv (not endpoint handle)
    ucs_status_ptr_t status = ucp_tag_recv_nbx(
        static_cast<ucp_worker_h>(impl_->worker_->native_handle()),
        buffer,
        length,
        tag,
        tag_mask,
        &params);

    if (status == nullptr) {
        // Immediate completion - this shouldn't happen for recv
        return Future<std::pair<size_t, Tag>>::make_ready({0, 0});
    }

    if (UCS_PTR_IS_ERR(status)) {
        ucs_status_t err = UCS_PTR_STATUS(status);
        return Future<std::pair<size_t, Tag>>::make_error(
            Status(ErrorCode::kTransportError, std::string("Recv failed: ") + ucs_status_string(err)));
    }

    // Async - for now return ready (would need request tracking)
    return Future<std::pair<size_t, Tag>>::make_ready({0, 0});
}

// RDMA
Future<void> Endpoint::put(const MemoryRegion::Ptr& /*local*/, size_t /*local_offset*/,
                         uint64_t /*remote_addr*/, const RemoteKey& /*rkey*/,
                         size_t /*length*/) {
    return Future<void>::make_error(
        Status(ErrorCode::kNotImplemented, "RDMA put requires connected endpoint"));
}

Future<void> Endpoint::get(const MemoryRegion::Ptr& /*local*/, size_t /*local_offset*/,
                          uint64_t /*remote_addr*/, const RemoteKey& /*rkey*/,
                          size_t /*length*/) {
    return Future<void>::make_error(
        Status(ErrorCode::kNotImplemented, "RDMA get requires connected endpoint"));
}

// Simplified put: entire local region
Future<void> Endpoint::put(const MemoryRegion::Ptr& local,
                         uint64_t remote_addr,
                         const RemoteKey& rkey) {
    if (!local) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument, "Invalid memory region"));
    }
    return put(local, 0, remote_addr, rkey, local->length());
}

// Simplified get: entire local region
Future<void> Endpoint::get(const MemoryRegion::Ptr& local,
                         uint64_t remote_addr,
                         const RemoteKey& rkey) {
    if (!local) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument, "Invalid memory region"));
    }
    return get(local, 0, remote_addr, rkey, local->length());
}

// Simplified atomic_fadd: without local buffer
Future<uint64_t> Endpoint::atomic_fadd(uint64_t /*remote_addr*/,
                                     const RemoteKey& /*rkey*/,
                                     uint64_t /*value*/) {
    return Future<uint64_t>::make_error(
        Status(ErrorCode::kNotImplemented, "Atomic operations not available in UCX 1.20.0"));
}

// Atomic compare-and-swap
Future<uint64_t> Endpoint::atomic_cswap(uint64_t /*remote_addr*/,
                                       const RemoteKey& /*rkey*/,
                                       uint64_t /*expected*/,
                                       uint64_t /*desired*/) {
    return Future<uint64_t>::make_error(
        Status(ErrorCode::kNotImplemented, "Atomic operations not available in UCX 1.20.0"));
}

Future<void> Endpoint::flush() {
    if (!impl_ || !impl_->handle_) {
        return Future<void>::make_ready();
    }

    ucs_status_ptr_t status = ucp_ep_flush_nbx(impl_->handle_, 0);

    if (status == nullptr) {
        return Future<void>::make_ready();
    }

    if (UCS_PTR_IS_ERR(status)) {
        ucs_status_t err = UCS_PTR_STATUS(status);
        return Future<void>::make_error(
            Status(ErrorCode::kTransportError, std::string("Flush failed: ") + ucs_status_string(err)));
    }

    return Future<void>::make_ready();
}

// Stream operations
Future<void> Endpoint::stream_send(const void* buffer, size_t length) {
    if (!impl_ || !impl_->handle_ || !buffer || length == 0) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument, "Invalid parameters"));
    }

    ucp_request_param_t params = {};
    params.op_attr_mask = UCP_OP_ATTR_FLAG_NO_IMM_CMPL;

    ucs_status_ptr_t status = ucp_stream_send_nbx(
        impl_->handle_,
        buffer,
        length,
        &params);

    if (status == nullptr) {
        return Future<void>::make_ready();
    }

    if (UCS_PTR_IS_ERR(status)) {
        ucs_status_t err = UCS_PTR_STATUS(status);
        return Future<void>::make_error(
            Status(ErrorCode::kTransportError, std::string("Stream send failed: ") + ucs_status_string(err)));
    }

    return Future<void>::make_ready();
}

Future<void> Endpoint::stream_send(const MemoryRegion::Ptr& /*region*/) {
    return Future<void>::make_error(
        Status(ErrorCode::kNotImplemented, "stream_send from region not implemented"));
}

Future<size_t> Endpoint::stream_recv(void* buffer, size_t length) {
    if (!impl_ || !impl_->handle_ || !buffer || length == 0) {
        return Future<size_t>::make_error(
            Status(ErrorCode::kInvalidArgument, "Invalid parameters"));
    }

    size_t received = 0;
    ucp_request_param_t params = {};
    params.op_attr_mask = UCP_OP_ATTR_FLAG_NO_IMM_CMPL;

    ucs_status_ptr_t status = ucp_stream_recv_nbx(
        impl_->handle_,
        buffer,
        length,
        &received,
        &params);

    if (status == nullptr) {
        return Future<size_t>::make_ready(received);
    }

    if (UCS_PTR_IS_ERR(status)) {
        ucs_status_t err = UCS_PTR_STATUS(status);
        return Future<size_t>::make_error(
            Status(ErrorCode::kTransportError, std::string("Stream recv failed: ") + ucs_status_string(err)));
    }

    return Future<size_t>::make_ready(received);
}

Future<size_t> Endpoint::stream_recv(const MemoryRegion::Ptr& /*region*/) {
    return Future<size_t>::make_error(
        Status(ErrorCode::kNotImplemented, "stream_recv to region not implemented"));
}

// Atomic
Future<uint64_t> Endpoint::atomic_fadd(const MemoryRegion::Ptr& /*local*/, size_t /*offset*/,
                                       uint64_t /*value*/, uint64_t /*remote_addr*/,
                                       const RemoteKey& /*rkey*/) {
    return Future<uint64_t>::make_error(
        Status(ErrorCode::kNotImplemented, "atomic not supported in UCX 1.20.0"));
}

// Lifecycle
Future<void> Endpoint::close() {
    if (!impl_ || !impl_->handle_) {
        return Future<void>::make_ready();
    }

    ucs_status_ptr_t status_ptr = ucp_ep_close_nbx(impl_->handle_, 0);

    if (status_ptr == nullptr) {
        // Immediate close
        impl_->handle_ = nullptr;
        return Future<void>::make_ready();
    }

    if (UCS_PTR_IS_ERR(status_ptr)) {
        ucs_status_t err = UCS_PTR_STATUS(status_ptr);
        impl_->handle_ = nullptr;
        return Future<void>::make_error(
            Status(ErrorCode::kEndpointClosed, std::string("Close failed: ") + ucs_status_string(err)));
    }

    // Async close - need to wait for completion
    // For now, return a ready future (UCX will complete in background)
    // A full implementation would track the close request
    impl_->handle_ = nullptr;
    return Future<void>::make_ready();
}

bool Endpoint::is_connected() const noexcept {
    return impl_ && impl_->handle_ != nullptr;
}

std::string Endpoint::remote_address() const {
    // UCX doesn't directly expose peer address as string
    // For now, return empty string
    return {};
}

void Endpoint::set_error_callback(std::function<void(Status)> /*cb*/) {
    // TODO: Implement error callback
}

Worker::Ptr Endpoint::worker() const noexcept {
    return impl_ ? impl_->worker_ : nullptr;
}

void* Endpoint::native_handle() const noexcept {
    return impl_ ? reinterpret_cast<void*>(impl_->handle_) : nullptr;
}

} // namespace p2p
