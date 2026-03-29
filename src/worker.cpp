#include "axon/worker.h"

#include "axon/config.h"
#include "axon/endpoint.h"
#include "axon/common.h"

#include <ucp/api/ucp.h>

#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <poll.h>

namespace axon {

namespace {

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
// Worker::Impl
// ============================================================================

struct Worker::Impl {
    Context::Ptr context_;
    ucp_worker_h handle_ = nullptr;
    int event_fd_ = -1;
    size_t index_ = 0;

    // Background progress thread
    std::thread progress_thread_;
    std::atomic<bool> progress_thread_running_{false};
    std::atomic<bool> progress_thread_stop_{false};
    std::mutex progress_mutex_;
    std::condition_variable progress_cv_;

    ~Impl() {
        // Ensure thread is stopped before destruction
        if (progress_thread_running_.load()) {
            progress_thread_stop_.store(true);
            progress_cv_.notify_all();
            if (progress_thread_.joinable()) {
                progress_thread_.join();
            }
        }
        if (handle_) {
            ucp_worker_destroy(handle_);
        }
    }
};

// ============================================================================
// Listener::Impl
// ============================================================================

struct Listener::Impl {
    Worker::Ptr worker_;
    ucp_listener_h handle_ = nullptr;
    std::string address_;
    AcceptCallback on_accept_;

    ~Impl() {
        if (handle_) {
            ucp_listener_destroy(handle_);
        }
    }
};

// ============================================================================
// Connection handler — defined after Listener::Impl is complete
// ============================================================================

static void connection_handler(ucp_conn_request_h conn_request, void* arg) {
    auto* listener_impl = static_cast<Listener::Impl*>(arg);
    if (!listener_impl) return;

    // Check if there is an accept callback before creating the endpoint
    if (!listener_impl->on_accept_) {
        // No callback registered — reject the connection
        ucp_listener_reject(listener_impl->handle_, conn_request);
        return;
    }

    ucp_worker_h worker = static_cast<ucp_worker_h>(listener_impl->worker_->native_handle());

    // Create server-side endpoint from the connection request
    ucp_ep_params_t ep_params = {};
    ep_params.field_mask = UCP_EP_PARAM_FIELD_CONN_REQUEST;
    ep_params.conn_request = conn_request;

    ucp_ep_h ep_handle = nullptr;
    ucs_status_t status = ucp_ep_create(worker, &ep_params, &ep_handle);
    if (status != UCS_OK) {
        return;
    }

    auto ep = Endpoint::create(listener_impl->worker_, ep_handle);

    // Invoke the accept callback (guaranteed non-null due to check above)
    listener_impl->on_accept_(ep);
}

// ============================================================================
// Worker
// ============================================================================

Worker::Ptr Worker::create(const Context::Ptr& ctx, size_t index) {
    if (!ctx || !ctx->native_handle()) {
        return nullptr;
    }

    ucp_worker_params_t params = {};
    params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    params.thread_mode = UCS_THREAD_MODE_MULTI;  // Allow concurrent access from multiple threads

    ucp_worker_h worker = nullptr;
    ucs_status_t status = ucp_worker_create(
        static_cast<ucp_context_h>(ctx->native_handle()),
        &params, &worker);

    if (status != UCS_OK) {
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->context_ = ctx;
    impl->handle_ = worker;
    impl->index_ = index;
    impl->event_fd_ = -1;  // Will be set when needed

    auto result = Ptr(new Worker(ctx, index));
    result->impl_ = std::move(impl);
    return result;
}

Worker::Worker(const Context::Ptr& ctx, size_t index)
    : impl_(std::make_unique<Impl>()) {
    impl_->context_ = ctx;
    impl_->index_ = index;
}

Worker::~Worker() = default;

bool Worker::progress() noexcept {
    return ucp_worker_progress(impl_->handle_) > 0;
}

bool Worker::wait(std::chrono::milliseconds timeout) {
    // Simple implementation: poll until ready or timeout
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (progress()) {
            return true;
        }
        // Could use epoll_wait on event_fd_ here for efficiency
    }
    return false;
}

// run_until is defined inline in worker.h as template

void Worker::run() {
    run_stop_.store(false);
    run_until([] { return false; });  // Infinite loop
}

void Worker::stop() noexcept {
    run_stop_.store(true);
}

// Background progress thread
void Worker::start_progress_thread() {
    if (impl_->progress_thread_running_.load()) {
        return;  // Already running
    }

    impl_->progress_thread_stop_.store(false);
    impl_->progress_thread_running_.store(true);  // Set before starting thread

    impl_->progress_thread_ = std::thread([this]() {
        while (!impl_->progress_thread_stop_.load()) {
            if (ucp_worker_progress(impl_->handle_) == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        impl_->progress_thread_running_.store(false);
    });
}

void Worker::stop_progress_thread() {
    if (!impl_->progress_thread_running_.load()) {
        return;  // Not running
    }

    impl_->progress_thread_stop_.store(true);

    if (impl_->progress_thread_.joinable()) {
        impl_->progress_thread_.join();
    }
}

bool Worker::is_progress_thread_running() const noexcept {
    return impl_->progress_thread_running_.load();
}

// Worker-level tag recv (for any-source receives)
Future<std::pair<size_t, Tag>> Worker::tag_recv(void* buffer, size_t length,
                                                Tag tag, Tag tag_mask) {
    if (!impl_ || !impl_->handle_ || !buffer || length == 0) {
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

    ucs_status_ptr_t status = ucp_tag_recv_nbx(
        impl_->handle_,
        buffer,
        length,
        tag,
        tag_mask,
        &params);

    if (status == nullptr) {
        return Future<std::pair<size_t, Tag>>::make_ready({0, 0});
    }

    if (UCS_PTR_IS_ERR(status)) {
        ucs_status_t err = UCS_PTR_STATUS(status);
        return Future<std::pair<size_t, Tag>>::make_error(
            Status(ErrorCode::kTransportError, std::string("Worker recv failed: ") + ucs_status_string(err)));
    }

    // Valid request pointer - operation is pending
    auto req = Request::create(status, impl_->handle_, bytes, matched_tag, callback_state);
    return Future<std::pair<size_t, Tag>>::make_request(req);
}

Future<std::pair<size_t, Tag>> Worker::tag_recv(const MemoryRegion::Ptr& region,
                                                Tag tag, Tag tag_mask) {
    (void)region;
    (void)tag;
    (void)tag_mask;
    if (!impl_ || !impl_->handle_) {
        return Future<std::pair<size_t, Tag>>::make_error(
            Status(ErrorCode::kInvalidArgument, "Invalid parameters"));
    }
    return Future<std::pair<size_t, Tag>>::make_error(
        Status(ErrorCode::kNotImplemented, "tag_recv into MemoryRegion is not implemented"));
}

Future<std::shared_ptr<Endpoint>> Worker::connect(const std::vector<uint8_t>& remote_address) {
    if (!impl_ || !impl_->handle_) {
        return Future<std::shared_ptr<Endpoint>>::make_error(
            Status(ErrorCode::kInvalidArgument, "Worker not initialized"));
    }
    if (remote_address.empty()) {
        return Future<std::shared_ptr<Endpoint>>::make_error(
            Status(ErrorCode::kInvalidArgument, "Remote address is empty"));
    }

    ucp_ep_params_t ep_params = {};
    ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
    ep_params.address = reinterpret_cast<const ucp_address_t*>(remote_address.data());

    ucp_ep_h ep_handle = nullptr;
    ucs_status_t status = ucp_ep_create(impl_->handle_, &ep_params, &ep_handle);

    if (status != UCS_OK) {
        return Future<std::shared_ptr<Endpoint>>::make_error(
            Status(ErrorCode::kTransportError,
                   std::string("Failed to create endpoint: ") + ucs_status_string(status)));
    }

    auto ep = Endpoint::create(shared_from_this(), ep_handle);
    return Future<std::shared_ptr<Endpoint>>::make_ready(ep);
}

Future<std::shared_ptr<Endpoint>> Worker::connect(const std::string& address) {
    if (!impl_ || !impl_->handle_) {
        return Future<std::shared_ptr<Endpoint>>::make_error(
            Status(ErrorCode::kInvalidArgument, "Worker not initialized"));
    }

    // Parse address: "host:port"
    std::string host;
    uint16_t port = 0;

    size_t colon_pos = address.rfind(':');
    if (colon_pos != std::string::npos) {
        host = address.substr(0, colon_pos);
        try {
            int port_int = std::stoi(address.substr(colon_pos + 1));
            if (port_int < 1 || port_int > 65535) {
                return Future<std::shared_ptr<Endpoint>>::make_error(
                    Status(ErrorCode::kInvalidArgument, "Port must be between 1 and 65535"));
            }
            port = static_cast<uint16_t>(port_int);
        } catch (...) {
            return Future<std::shared_ptr<Endpoint>>::make_error(
                Status(ErrorCode::kInvalidArgument, "Invalid port in address"));
        }
    } else {
        return Future<std::shared_ptr<Endpoint>>::make_error(
            Status(ErrorCode::kInvalidArgument, "Address must be in host:port format"));
    }

    // Build sockaddr_in
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return Future<std::shared_ptr<Endpoint>>::make_error(
            Status(ErrorCode::kInvalidArgument,
                   std::string("Invalid IP address: ") + host));
    }

    // Create endpoint using sockaddr (client-server connection)
    ucp_ep_params_t ep_params = {};
    ep_params.field_mask = UCP_EP_PARAM_FIELD_FLAGS |
                           UCP_EP_PARAM_FIELD_SOCK_ADDR;
    ep_params.flags = UCP_EP_PARAMS_FLAGS_CLIENT_SERVER;
    ep_params.sockaddr.addr = reinterpret_cast<struct sockaddr*>(&addr);
    ep_params.sockaddr.addrlen = sizeof(addr);

    ucp_ep_h ep_handle = nullptr;
    ucs_status_t status = ucp_ep_create(impl_->handle_, &ep_params, &ep_handle);

    if (status != UCS_OK) {
        return Future<std::shared_ptr<Endpoint>>::make_error(
            Status(ErrorCode::kTransportError,
                   std::string("Failed to connect to ") + address + ": " + ucs_status_string(status)));
    }

    auto ep = Endpoint::create(shared_from_this(), ep_handle);
    return Future<std::shared_ptr<Endpoint>>::make_ready(ep);
}

Listener::Ptr Worker::listen(const std::string& address,
                            AcceptCallback on_accept) {
    // Parse address
    std::string host;
    uint16_t port = 0;

    size_t colon_pos = address.find(':');
    if (colon_pos != std::string::npos) {
        host = address.substr(0, colon_pos);
        try {
            port = static_cast<uint16_t>(std::stoi(address.substr(colon_pos + 1)));
        } catch (...) {
            return nullptr;
        }
    } else {
        // If no port specified, use 0 for OS-assigned port
        host = address;
    }

    // Resolve host to IP address
    if (host.empty() || host == "0.0.0.0" || host == "::") {
        host = "0.0.0.0";
    }

    // Setup socket address for UCX listener
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (host == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    }

    // Pre-create the Listener::Impl so we can pass it as the callback arg
    auto listener_impl = std::make_unique<Listener::Impl>();
    listener_impl->worker_ = shared_from_this();
    listener_impl->on_accept_ = on_accept;  // copy before move

    // Create listener params with connection handler callback
    ucp_listener_params_t params = {};
    params.field_mask = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR |
                       UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;

    // Setup connection handler callback — pass the impl as arg
    params.conn_handler.cb = connection_handler;
    params.conn_handler.arg = listener_impl.get();

    // Create sockaddr length
    params.sockaddr.addrlen = sizeof(addr);
    params.sockaddr.addr = reinterpret_cast<struct sockaddr*>(&addr);

    ucp_listener_h listener = nullptr;
    ucs_status_t status = ucp_listener_create(impl_->handle_, &params, &listener);

    if (status != UCS_OK) {
        return nullptr;
    }

    // Create Listener wrapper
    auto listener_ptr = std::shared_ptr<Listener>(new Listener());
    listener_impl->handle_ = listener;
    listener_impl->on_accept_ = std::move(on_accept);
    listener_ptr->impl_ = std::move(listener_impl);

    // Get the actual bound address
    ucp_listener_attr_t attr = {};
    attr.field_mask = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR;
    ucp_listener_query(listener, &attr);
    if (attr.sockaddr.ss_family == AF_INET) {
        struct sockaddr_in* bound_addr = reinterpret_cast<struct sockaddr_in*>(&attr.sockaddr);
        char buf[32];
        inet_ntop(AF_INET, &bound_addr->sin_addr, buf, sizeof(buf));
        listener_ptr->impl_->address_ = std::string(buf) + ":" + std::to_string(ntohs(bound_addr->sin_port));
    }

    return listener_ptr;
}

int Worker::event_fd() const noexcept {
    return impl_->event_fd_;
}

std::vector<uint8_t> Worker::worker_address() const {
    ucp_address_t* address = nullptr;
    size_t length = 0;
    ucp_worker_get_address(impl_->handle_, &address, &length);

    std::vector<uint8_t> result(length);
    std::memcpy(result.data(), address, length);
    ucp_worker_release_address(impl_->handle_, address);

    return result;
}

std::vector<uint8_t> Worker::address() const {
    return worker_address();
}

void* Worker::native_handle() const noexcept {
    return impl_->handle_;
}

Context::Ptr Worker::context() const noexcept {
    return impl_->context_;
}

size_t Worker::index() const noexcept {
    return impl_ ? impl_->index_ : 0;
}

// ============================================================================
// Listener
// ============================================================================

Listener::~Listener() = default;

std::string Listener::address() const {
    if (impl_) {
        return impl_->address_;
    }
    return {};
}

void* Listener::native_handle() const noexcept {
    if (impl_) {
        return reinterpret_cast<void*>(impl_->handle_);
    }
    return nullptr;
}

Context::Ptr Listener::context() const noexcept {
    if (impl_) {
        return impl_->worker_->context();
    }
    return nullptr;
}

void Listener::add_connection_request(ucp_conn_request_h req) {
    (void)req;
    // Connection requests are handled via the global pending list
}

bool Listener::accept() {
    // Connections are accepted immediately in the connection_handler callback.
    // This polling interface is no longer needed — always returns false.
    return false;
}

void Listener::close() {
    if (impl_ && impl_->handle_) {
        ucp_listener_destroy(impl_->handle_);
        impl_->handle_ = nullptr;
    }
}

} // namespace axon
