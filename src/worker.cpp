#include "p2p/worker.h"

#include "p2p/config.h"
#include "p2p/endpoint.h"
#include "p2p/common.h"

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

namespace p2p {

// ============================================================================
// Connection request storage for accept callback
// ============================================================================

static std::map<ucp_conn_request_h, Listener::Ptr>* g_conn_request_listeners = nullptr;
static std::mutex g_conn_mutex;

static void connection_handler(ucp_conn_request_h conn_request, void* arg) {
    // Store the connection request for later acceptance
    // The callback is invoked when a connection request arrives
    (void)arg;
    std::lock_guard<std::mutex> lock(g_conn_mutex);
    if (g_conn_request_listeners) {
        // Just acknowledge the connection request for now
        // Full implementation would create endpoint from conn_request
        (*g_conn_request_listeners)[conn_request] = nullptr;
    }
}

// ============================================================================
// Worker::Impl
// ============================================================================

struct Worker::Impl {
    Context::Ptr context_;
    ucp_worker_h handle_ = nullptr;
    int event_fd_ = -1;
    size_t index_ = 0;

    ~Impl() {
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
// Worker
// ============================================================================

Worker::Ptr Worker::create(const Context::Ptr& ctx, size_t index) {
    if (!ctx || !ctx->native_handle()) {
        return nullptr;
    }

    ucp_worker_params_t params = {};
    params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE |
                       UCP_WORKER_PARAM_FIELD_EVENT_FD;
    params.thread_mode = UCS_THREAD_MODE_SINGLE;

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
    run_until([] { return false; });  // Infinite loop
}

void Worker::stop() noexcept {
    // Worker doesn't have explicit stop in UCX
    (void)this;
}

// Worker-level tag recv (for any-source receives)
Future<std::pair<size_t, Tag>> Worker::tag_recv(void* buffer, size_t length,
                                                Tag tag, Tag tag_mask) {
    if (!impl_ || !impl_->handle_ || !buffer || length == 0) {
        return Future<std::pair<size_t, Tag>>::make_error(
            Status(ErrorCode::kInvalidArgument, "Invalid parameters"));
    }

    ucp_request_param_t params = {};
    params.op_attr_mask = UCP_OP_ATTR_FLAG_NO_IMM_CMPL;

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

    return Future<std::pair<size_t, Tag>>::make_ready({0, 0});
}

Future<std::pair<size_t, Tag>> Worker::tag_recv(const MemoryRegion::Ptr& /*region*/,
                                                Tag /*tag*/, Tag /*tag_mask*/) {
    // TODO: Implement with memory region
    return Future<std::pair<size_t, Tag>>::make_error(
        Status(ErrorCode::kNotImplemented, "tag_recv from region not implemented"));
}

Future<std::shared_ptr<Endpoint>> Worker::connect(const std::vector<uint8_t>& /*remote_address*/) {
    // In UCX 1.20.0, endpoint creation with remote address requires
    // a different API. Return not implemented for now.
    return Future<std::shared_ptr<Endpoint>>::make_error(
        Status(ErrorCode::kNotImplemented, "connect with address blob not implemented in UCX 1.20.0"));
}

Future<std::shared_ptr<Endpoint>> Worker::connect(const std::string& address) {
    // Parse address: "tcp://host:port" or "host:port"
    std::string host;
    uint16_t port = 0;

    // Simple parsing - look for colon
    size_t colon_pos = address.find(':');
    if (colon_pos != std::string::npos) {
        host = address.substr(0, colon_pos);
        try {
            port = static_cast<uint16_t>(std::stoi(address.substr(colon_pos + 1)));
        } catch (...) {
            return Future<std::shared_ptr<Endpoint>>::make_error(
                Status(ErrorCode::kInvalidArgument, "Invalid port in address"));
        }
    } else {
        host = address;
    }

    // Get peer's worker address via DNS or service name resolution
    // For now, use a simplified approach - the actual implementation would
    // need a bootstrap mechanism (e.g., share addresses via file, environment, etc.)

    // TODO: Implement proper address resolution and connection
    // For now, return not implemented
    (void)host;
    (void)port;
    return Future<std::shared_ptr<Endpoint>>::make_error(
        Status(ErrorCode::kNotImplemented, "connect requires bootstrap mechanism"));
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

    // Initialize connection request storage if needed
    if (!g_conn_request_listeners) {
        g_conn_request_listeners = new std::map<ucp_conn_request_h, Listener::Ptr>();
    }

    // Create listener params with connection handler callback
    ucp_listener_params_t params = {};
    params.field_mask = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR |
                       UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;

    // Setup connection handler callback
    // Note: In UCX 1.20.0, the callback is invoked when connection request arrives
    // The callback will store conn_request for later processing
    params.conn_handler.cb = connection_handler;
    params.conn_handler.arg = nullptr;  // Will be set after listener is created

    // Create sockaddr length
    params.sockaddr.addrlen = sizeof(addr);
    params.sockaddr.addr = reinterpret_cast<struct sockaddr*>(&addr);

    ucp_listener_h listener = nullptr;
    ucs_status_t status = ucp_listener_create(impl_->handle_, &params, &listener);

    if (status != UCS_OK) {
        return nullptr;
    }

    // Create Listener wrapper using new + shared_ptr (requires public constructor)
    auto listener_ptr = std::shared_ptr<Listener>(new Listener());
    listener_ptr->impl_ = std::make_unique<Listener::Impl>();
    listener_ptr->impl_->worker_ = shared_from_this();
    listener_ptr->impl_->handle_ = listener;
    listener_ptr->impl_->on_accept_ = std::move(on_accept);

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

void Listener::close() {
    if (impl_ && impl_->handle_) {
        ucp_listener_destroy(impl_->handle_);
        impl_->handle_ = nullptr;
    }
}

} // namespace p2p
