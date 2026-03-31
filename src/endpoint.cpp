#include "zerokv/endpoint.h"

#include "zerokv/worker.h"
#include "zerokv/memory.h"
#include "zerokv/future.h"
#include "zerokv/common.h"

#include <ucp/api/ucp.h>
#include <memory>

namespace axon {

namespace {

struct RmaRequestState {
    MemoryRegion::Ptr region;
    ucp_rkey_h rkey = nullptr;

    ~RmaRequestState() {
        if (rkey != nullptr) {
            ucp_rkey_destroy(rkey);
        }
    }
};

struct AtomicRequestState {
    std::shared_ptr<size_t> result;
    uint64_t reply_value = 0;
    ucp_rkey_h rkey = nullptr;

    ~AtomicRequestState() {
        if (rkey != nullptr) {
            ucp_rkey_destroy(rkey);
        }
    }
};

void atomic_fetch_callback(void* /*request*/, ucs_status_t status, void* user_data) {
    auto* state = static_cast<AtomicRequestState*>(user_data);
    if (state && state->result) {
        if (status == UCS_OK) {
            *state->result = static_cast<size_t>(state->reply_value);
        } else {
            *state->result = 0;
        }
    }
}

void stream_recv_callback(void* /*request*/, ucs_status_t status, size_t length, void* user_data) {
    auto* bytes = static_cast<size_t*>(user_data);
    if (bytes != nullptr) {
        *bytes = (status == UCS_OK) ? length : 0;
    }
}

struct TagRecvCallbackState {
    size_t* bytes = nullptr;
    Tag* tag = nullptr;
};

void tag_recv_callback(void* /*request*/, ucs_status_t status,
                       const ucp_tag_recv_info_t* tag_info, void* user_data) {
    auto* state = static_cast<TagRecvCallbackState*>(user_data);
    if (state == nullptr || state->bytes == nullptr || state->tag == nullptr) {
        return;
    }

    if (status == UCS_OK && tag_info != nullptr) {
        *state->bytes = tag_info->length;
        *state->tag = tag_info->sender_tag;
    } else {
        *state->bytes = 0;
        *state->tag = 0;
    }
}

}  // namespace

// ============================================================================
// Endpoint::Impl
// ============================================================================

struct Endpoint::Impl {
    Worker::Ptr worker_;
    ucp_ep_h handle_ = nullptr;
    std::function<void(Status)> error_callback_;

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

    // Valid request pointer - operation is pending
    auto req = Request::create(status, static_cast<ucp_worker_h>(impl_->worker_->native_handle()));
    return Future<void>::make_request(req);
}

Future<void> Endpoint::tag_send(const MemoryRegion::Ptr& region, size_t offset,
                                size_t length, Tag tag) {
    if (!impl_ || !impl_->handle_ || !region) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument, "Invalid parameters"));
    }
    if (offset + length > region->length()) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument, "Offset + length exceeds region size"));
    }

    ucp_request_param_t params = {};
    params.op_attr_mask = UCP_OP_ATTR_FLAG_NO_IMM_CMPL;

    ucs_status_ptr_t status = ucp_tag_send_nbx(
        impl_->handle_,
        static_cast<char*>(region->address()) + offset,
        length,
        tag,
        &params);

    if (status == nullptr) {
        return Future<void>::make_ready();
    }

    if (UCS_PTR_IS_ERR(status)) {
        ucs_status_t err = UCS_PTR_STATUS(status);
        return Future<void>::make_error(
            Status(ErrorCode::kTransportError, std::string("Send from region failed: ") + ucs_status_string(err)));
    }

    // Keep MemoryRegion alive for async operation
    auto req = Request::create(status, static_cast<ucp_worker_h>(impl_->worker_->native_handle()),
                               0, std::shared_ptr<void>(region, region.get()));
    return Future<void>::make_request(req);
}

Future<std::pair<size_t, Tag>> Endpoint::tag_recv(void* buffer, size_t length,
                                                 Tag tag, Tag tag_mask) {
    if (!impl_ || !impl_->worker_ || !buffer || length == 0) {
        return Future<std::pair<size_t, Tag>>::make_error(
            Status(ErrorCode::kInvalidArgument, "Invalid parameters"));
    }

    auto bytes = std::make_shared<size_t>(0);
    auto matched_tag = std::make_shared<Tag>(0);
    auto callback_state = std::make_shared<TagRecvCallbackState>();
    callback_state->bytes = bytes.get();
    callback_state->tag = matched_tag.get();

    ucp_request_param_t params = {};
    params.op_attr_mask = UCP_OP_ATTR_FLAG_NO_IMM_CMPL |
                          UCP_OP_ATTR_FIELD_CALLBACK |
                          UCP_OP_ATTR_FIELD_USER_DATA;
    params.cb.recv = tag_recv_callback;
    params.user_data = callback_state.get();

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

    // Valid request pointer - operation is pending
    auto req = Request::create(status, static_cast<ucp_worker_h>(impl_->worker_->native_handle()),
                               bytes, matched_tag, callback_state);
    return Future<std::pair<size_t, Tag>>::make_request(req);
}

Future<std::pair<size_t, Tag>> Endpoint::tag_recv(const MemoryRegion::Ptr& region,
                                                  Tag tag, Tag tag_mask) {
    (void)region;
    (void)tag;
    (void)tag_mask;
    if (!impl_ || !impl_->worker_) {
        return Future<std::pair<size_t, Tag>>::make_error(
            Status(ErrorCode::kInvalidArgument, "Invalid parameters"));
    }
    return Future<std::pair<size_t, Tag>>::make_error(
        Status(ErrorCode::kNotImplemented, "tag_recv into MemoryRegion is not implemented"));
}

// RDMA
Future<void> Endpoint::put(const MemoryRegion::Ptr& local, size_t local_offset,
                         uint64_t remote_addr, const RemoteKey& rkey,
                         size_t length) {
    if (!impl_ || !impl_->handle_) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument, "Endpoint not connected"));
    }
    if (!local || local_offset + length > local->length()) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument, "Invalid local region or offset"));
    }
    if (rkey.empty()) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument, "Empty remote key"));
    }

    // Unpack remote key
    ucp_rkey_h ucx_rkey = nullptr;
    ucs_status_t status = ucp_ep_rkey_unpack(impl_->handle_, rkey.bytes(), &ucx_rkey);
    if (status != UCS_OK) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument,
                   std::string("Failed to unpack remote key: ") + ucs_status_string(status)));
    }

    auto state = std::make_shared<RmaRequestState>();
    state->region = local;
    state->rkey = ucx_rkey;

    ucp_request_param_t params = {};
    params.op_attr_mask = UCP_OP_ATTR_FLAG_NO_IMM_CMPL;

    ucs_status_ptr_t req = ucp_put_nbx(
        impl_->handle_,
        static_cast<char*>(local->address()) + local_offset,
        length,
        remote_addr,
        ucx_rkey,
        &params);

    if (req == nullptr) {
        state.reset();
        return Future<void>::make_ready();
    }

    if (UCS_PTR_IS_ERR(req)) {
        ucs_status_t err = UCS_PTR_STATUS(req);
        state.reset();
        return Future<void>::make_error(
            Status(ErrorCode::kTransportError, std::string("RDMA put failed: ") + ucs_status_string(err)));
    }

    auto request = Request::create(req, static_cast<ucp_worker_h>(impl_->worker_->native_handle()),
                                   0, state);
    return Future<void>::make_request(request);
}

Future<void> Endpoint::get(const MemoryRegion::Ptr& local, size_t local_offset,
                          uint64_t remote_addr, const RemoteKey& rkey,
                          size_t length) {
    if (!impl_ || !impl_->handle_) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument, "Endpoint not connected"));
    }
    if (!local || local_offset + length > local->length()) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument, "Invalid local region or offset"));
    }
    if (rkey.empty()) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument, "Empty remote key"));
    }

    // Unpack remote key
    ucp_rkey_h ucx_rkey = nullptr;
    ucs_status_t status = ucp_ep_rkey_unpack(impl_->handle_, rkey.bytes(), &ucx_rkey);
    if (status != UCS_OK) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument,
                   std::string("Failed to unpack remote key: ") + ucs_status_string(status)));
    }

    auto state = std::make_shared<RmaRequestState>();
    state->region = local;
    state->rkey = ucx_rkey;

    ucp_request_param_t params = {};
    params.op_attr_mask = UCP_OP_ATTR_FLAG_NO_IMM_CMPL;

    ucs_status_ptr_t req = ucp_get_nbx(
        impl_->handle_,
        static_cast<char*>(local->address()) + local_offset,
        length,
        remote_addr,
        ucx_rkey,
        &params);

    if (req == nullptr) {
        state.reset();
        return Future<void>::make_ready();
    }

    if (UCS_PTR_IS_ERR(req)) {
        ucs_status_t err = UCS_PTR_STATUS(req);
        state.reset();
        return Future<void>::make_error(
            Status(ErrorCode::kTransportError, std::string("RDMA get failed: ") + ucs_status_string(err)));
    }

    auto request = Request::create(req, static_cast<ucp_worker_h>(impl_->worker_->native_handle()),
                                   0, state);
    return Future<void>::make_request(request);
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
Future<uint64_t> Endpoint::atomic_fadd(uint64_t remote_addr,
                                     const RemoteKey& rkey,
                                     uint64_t value) {
    if (!impl_ || !impl_->handle_) {
        return Future<uint64_t>::make_error(
            Status(ErrorCode::kInvalidArgument, "Endpoint not connected"));
    }
    if (rkey.empty()) {
        return Future<uint64_t>::make_error(
            Status(ErrorCode::kInvalidArgument, "Empty remote key"));
    }

    // Unpack remote key
    ucp_rkey_h ucx_rkey = nullptr;
    ucs_status_t status = ucp_ep_rkey_unpack(impl_->handle_, rkey.bytes(), &ucx_rkey);
    if (status != UCS_OK) {
        return Future<uint64_t>::make_error(
            Status(ErrorCode::kInvalidArgument,
                   std::string("Failed to unpack remote key: ") + ucs_status_string(status)));
    }

    auto state = std::make_shared<AtomicRequestState>();
    state->result = std::make_shared<size_t>(0);
    state->rkey = ucx_rkey;

    uint64_t add_value = value;

    ucp_request_param_t params = {};
    params.op_attr_mask = UCP_OP_ATTR_FLAG_NO_IMM_CMPL |
                          UCP_OP_ATTR_FIELD_REPLY_BUFFER |
                          UCP_OP_ATTR_FIELD_CALLBACK |
                          UCP_OP_ATTR_FIELD_USER_DATA;
    params.reply_buffer = &state->reply_value;
    params.cb.send = atomic_fetch_callback;
    params.user_data = state.get();

    ucs_status_ptr_t req = ucp_atomic_op_nbx(
        impl_->handle_,
        UCP_ATOMIC_OP_ADD,
        &add_value,
        1,
        remote_addr,
        ucx_rkey,
        &params);

    if (req == nullptr) {
        // Immediate completion - result is in reply_value
        uint64_t result = state->reply_value;
        return Future<uint64_t>::make_ready(result);
    }

    if (UCS_PTR_IS_ERR(req)) {
        ucs_status_t err = UCS_PTR_STATUS(req);
        return Future<uint64_t>::make_error(
            Status(ErrorCode::kTransportError,
                   std::string("Atomic fadd failed: ") + ucs_status_string(err)));
    }

    // Pending operation — reuse shared_ptr<size_t> overload, set is_atomic flag
    auto request = Request::create(req,
                                   static_cast<ucp_worker_h>(impl_->worker_->native_handle()),
                                   state->result, state);
    return Future<uint64_t>::make_request(request);
}

// Atomic compare-and-swap
Future<uint64_t> Endpoint::atomic_cswap(uint64_t remote_addr,
                                       const RemoteKey& rkey,
                                       uint64_t expected,
                                       uint64_t desired) {
    if (!impl_ || !impl_->handle_) {
        return Future<uint64_t>::make_error(
            Status(ErrorCode::kInvalidArgument, "Endpoint not connected"));
    }
    if (rkey.empty()) {
        return Future<uint64_t>::make_error(
            Status(ErrorCode::kInvalidArgument, "Empty remote key"));
    }

    // Unpack remote key
    ucp_rkey_h ucx_rkey = nullptr;
    ucs_status_t status = ucp_ep_rkey_unpack(impl_->handle_, rkey.bytes(), &ucx_rkey);
    if (status != UCS_OK) {
        return Future<uint64_t>::make_error(
            Status(ErrorCode::kInvalidArgument,
                   std::string("Failed to unpack remote key: ") + ucs_status_string(status)));
    }

    auto state = std::make_shared<AtomicRequestState>();
    state->result = std::make_shared<size_t>(0);
    state->rkey = ucx_rkey;

    // For CSWAP, buffer layout: [expected, desired]
    uint64_t cswap_buf[2] = {expected, desired};

    ucp_request_param_t params = {};
    params.op_attr_mask = UCP_OP_ATTR_FLAG_NO_IMM_CMPL |
                          UCP_OP_ATTR_FIELD_REPLY_BUFFER |
                          UCP_OP_ATTR_FIELD_CALLBACK |
                          UCP_OP_ATTR_FIELD_USER_DATA;
    params.reply_buffer = &state->reply_value;
    params.cb.send = atomic_fetch_callback;
    params.user_data = state.get();

    ucs_status_ptr_t req = ucp_atomic_op_nbx(
        impl_->handle_,
        UCP_ATOMIC_OP_CSWAP,
        cswap_buf,
        2,
        remote_addr,
        ucx_rkey,
        &params);

    if (req == nullptr) {
        // Immediate completion
        uint64_t result = state->reply_value;
        return Future<uint64_t>::make_ready(result);
    }

    if (UCS_PTR_IS_ERR(req)) {
        ucs_status_t err = UCS_PTR_STATUS(req);
        return Future<uint64_t>::make_error(
            Status(ErrorCode::kTransportError,
                   std::string("Atomic cswap failed: ") + ucs_status_string(err)));
    }

    // Pending operation — reuse shared_ptr<size_t> overload
    auto request = Request::create(req,
                                   static_cast<ucp_worker_h>(impl_->worker_->native_handle()),
                                   state->result, state);
    return Future<uint64_t>::make_request(request);
}

Future<void> Endpoint::flush() {
    if (!impl_ || !impl_->handle_) {
        return Future<void>::make_ready();
    }

    ucp_request_param_t params = {};
    params.op_attr_mask = UCP_OP_ATTR_FLAG_NO_IMM_CMPL;

    ucs_status_ptr_t status = ucp_ep_flush_nbx(impl_->handle_, &params);

    if (status == nullptr) {
        return Future<void>::make_ready();
    }

    if (UCS_PTR_IS_ERR(status)) {
        ucs_status_t err = UCS_PTR_STATUS(status);
        return Future<void>::make_error(
            Status(ErrorCode::kTransportError, std::string("Flush failed: ") + ucs_status_string(err)));
    }

    auto req = Request::create(status, static_cast<ucp_worker_h>(impl_->worker_->native_handle()));
    return Future<void>::make_request(req);
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

    auto req = Request::create(status, static_cast<ucp_worker_h>(impl_->worker_->native_handle()));
    return Future<void>::make_request(req);
}

Future<void> Endpoint::stream_send(const MemoryRegion::Ptr& region) {
    if (!impl_ || !impl_->handle_ || !region) {
        return Future<void>::make_error(
            Status(ErrorCode::kInvalidArgument, "Invalid parameters"));
    }

    ucp_request_param_t params = {};
    params.op_attr_mask = UCP_OP_ATTR_FLAG_NO_IMM_CMPL;

    ucs_status_ptr_t status = ucp_stream_send_nbx(
        impl_->handle_,
        region->address(),
        region->length(),
        &params);

    if (status == nullptr) {
        return Future<void>::make_ready();
    }

    if (UCS_PTR_IS_ERR(status)) {
        ucs_status_t err = UCS_PTR_STATUS(status);
        return Future<void>::make_error(
            Status(ErrorCode::kTransportError,
                   std::string("Stream send from region failed: ") + ucs_status_string(err)));
    }

    // Keep MemoryRegion alive for async operation
    auto req = Request::create(status, static_cast<ucp_worker_h>(impl_->worker_->native_handle()),
                               0, std::shared_ptr<void>(region, region.get()));
    return Future<void>::make_request(req);
}

Future<size_t> Endpoint::stream_recv(void* buffer, size_t length) {
    if (!impl_ || !impl_->handle_ || !buffer || length == 0) {
        return Future<size_t>::make_error(
            Status(ErrorCode::kInvalidArgument, "Invalid parameters"));
    }

    auto received = std::make_shared<size_t>(0);
    ucp_request_param_t params = {};
    params.op_attr_mask = UCP_OP_ATTR_FLAG_NO_IMM_CMPL |
                          UCP_OP_ATTR_FIELD_CALLBACK |
                          UCP_OP_ATTR_FIELD_USER_DATA;
    params.cb.recv_stream = stream_recv_callback;
    params.user_data = received.get();

    ucs_status_ptr_t status = ucp_stream_recv_nbx(
        impl_->handle_,
        buffer,
        length,
        received.get(),
        &params);

    if (status == nullptr) {
        return Future<size_t>::make_ready(*received);
    }

    if (UCS_PTR_IS_ERR(status)) {
        ucs_status_t err = UCS_PTR_STATUS(status);
        return Future<size_t>::make_error(
            Status(ErrorCode::kTransportError, std::string("Stream recv failed: ") + ucs_status_string(err)));
    }

    auto req = Request::create(status, static_cast<ucp_worker_h>(impl_->worker_->native_handle()),
                               received);
    return Future<size_t>::make_request(req);
}

Future<size_t> Endpoint::stream_recv(const MemoryRegion::Ptr& region) {
    if (!impl_ || !impl_->handle_ || !region) {
        return Future<size_t>::make_error(
            Status(ErrorCode::kInvalidArgument, "Invalid parameters"));
    }

    auto received = std::make_shared<size_t>(0);
    ucp_request_param_t params = {};
    params.op_attr_mask = UCP_OP_ATTR_FLAG_NO_IMM_CMPL |
                          UCP_OP_ATTR_FIELD_CALLBACK |
                          UCP_OP_ATTR_FIELD_USER_DATA;
    params.cb.recv_stream = stream_recv_callback;
    params.user_data = received.get();

    ucs_status_ptr_t status = ucp_stream_recv_nbx(
        impl_->handle_,
        region->address(),
        region->length(),
        received.get(),
        &params);

    if (status == nullptr) {
        return Future<size_t>::make_ready(*received);
    }

    if (UCS_PTR_IS_ERR(status)) {
        ucs_status_t err = UCS_PTR_STATUS(status);
        return Future<size_t>::make_error(
            Status(ErrorCode::kTransportError,
                   std::string("Stream recv to region failed: ") + ucs_status_string(err)));
    }

    auto req = Request::create(status, static_cast<ucp_worker_h>(impl_->worker_->native_handle()),
                               received, std::shared_ptr<void>(region, region.get()));
    return Future<size_t>::make_request(req);
}

// Atomic with local buffer
Future<uint64_t> Endpoint::atomic_fadd(const MemoryRegion::Ptr& local, size_t offset,
                                       uint64_t value, uint64_t remote_addr,
                                       const RemoteKey& rkey) {
    (void)local;
    (void)offset;
    // Delegate to simplified version
    return atomic_fadd(remote_addr, rkey, value);
}

// Lifecycle
Future<void> Endpoint::close() {
    if (!impl_ || !impl_->handle_) {
        return Future<void>::make_ready();
    }

    ucp_request_param_t params = {};
    params.op_attr_mask = UCP_OP_ATTR_FLAG_NO_IMM_CMPL;

    ucs_status_ptr_t status_ptr = ucp_ep_close_nbx(impl_->handle_, &params);

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

    auto req = Request::create(status_ptr, static_cast<ucp_worker_h>(impl_->worker_->native_handle()));
    impl_->handle_ = nullptr;
    return Future<void>::make_request(req);
}

bool Endpoint::is_connected() const noexcept {
    return impl_ && impl_->handle_ != nullptr;
}

std::string Endpoint::remote_address() const {
    // UCX doesn't directly expose peer address as string
    // For now, return empty string
    return {};
}

void Endpoint::set_error_callback(std::function<void(Status)> cb) {
    if (impl_) {
        impl_->error_callback_ = std::move(cb);
    }
}

Worker::Ptr Endpoint::worker() const noexcept {
    return impl_ ? impl_->worker_ : nullptr;
}

void* Endpoint::native_handle() const noexcept {
    return impl_ ? reinterpret_cast<void*>(impl_->handle_) : nullptr;
}

} // namespace axon
