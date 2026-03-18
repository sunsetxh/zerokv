#pragma once

/// @file p2p/future.h
/// @brief Asynchronous operation primitives: Request, Future<T>, callback support.
///
/// Design rationale:
///   - Request is the low-level handle representing one in-flight UCX operation.
///   - Future<T> wraps Request with a typed, composable async result.
///   - Callbacks are optional; users may poll, wait, or compose futures.
///   - Inspired by std::future but non-blocking by default.

#include "p2p/common.h"

#include <ucp/api/ucp.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <variant>

namespace p2p {

// ---------------------------------------------------------------------------
// Request – low-level async handle
// ---------------------------------------------------------------------------

/// Represents a single in-flight operation (send, recv, put, get, etc.).
/// Wraps a UCX ucs_status_ptr_t internally.
class Request {
public:
    using Ptr = std::shared_ptr<Request>;

    ~Request();

    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;
    Request(Request&&) noexcept;
    Request& operator=(Request&&) noexcept;

    /// Check completion without blocking.
    [[nodiscard]] bool is_complete() const noexcept;

    /// Current status (kInProgress while pending).
    [[nodiscard]] Status status() const noexcept;

    /// Block until complete or timeout.
    /// Returns the final Status.
    Status wait(std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});

    /// Attempt to cancel the operation.
    /// Cancellation is best-effort; the completion callback will still fire
    /// with status kCanceled.
    void cancel();

    /// Number of bytes actually transferred (valid after completion).
    [[nodiscard]] size_t bytes_transferred() const noexcept;

    /// The tag that was matched (valid after recv completion).
    [[nodiscard]] Tag matched_tag() const noexcept;

    /// Native UCX request pointer.
    [[nodiscard]] void* native_handle() const noexcept;

    static Ptr create(void* ucx_request, ucp_worker_h worker, size_t initial_bytes = 0);

    // For operations with MemoryRegion that needs to keep it alive
    static Ptr create(void* ucx_request, ucp_worker_h worker, size_t initial_bytes,
                      std::shared_ptr<void> keep_alive);

    // For operations that report transferred bytes via async callbacks.
    static Ptr create(void* ucx_request, ucp_worker_h worker,
                      std::shared_ptr<size_t> async_bytes_transferred,
                      std::shared_ptr<void> keep_alive = {});

    static Ptr create(void* ucx_request, ucp_worker_h worker,
                      std::shared_ptr<size_t> async_bytes_transferred,
                      std::shared_ptr<Tag> async_matched_tag,
                      std::shared_ptr<void> keep_alive = {});

private:
    friend class Worker;
    friend class Endpoint;
    Request() = default;
    void populate_recv_info() const;
    struct Impl {
        void* ucx_request_ = nullptr;
        ucp_worker_h worker_ = nullptr;
        Status status_;
        mutable size_t bytes_transferred_ = 0;
        mutable Tag matched_tag_ = 0;
        std::shared_ptr<size_t> async_bytes_transferred_;
        std::shared_ptr<Tag> async_matched_tag_;
        std::shared_ptr<void> keep_alive_;  // Keeps MemoryRegion alive for async ops
    };
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Completion callback
// ---------------------------------------------------------------------------

/// Signature for completion callbacks.  Receives the final Status and the
/// number of bytes transferred.
using CompletionCallback = std::function<void(Status status, size_t bytes)>;

// ---------------------------------------------------------------------------
// Future<T> – typed async result
// ---------------------------------------------------------------------------

/// A non-blocking future that wraps an async operation.
///
/// @tparam T  The result type.  `void` for fire-and-forget sends.
///            `size_t` for recv (bytes received).
///            `std::pair<size_t, Tag>` for tag_recv (bytes + matched tag).
template <typename T>
class Future {
public:
    Future() = default;
    ~Future() = default;

    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;
    Future(Future&&) noexcept = default;
    Future& operator=(Future&&) noexcept = default;

    /// Non-blocking poll.
    [[nodiscard]] bool ready() const noexcept { 
        if (request_ && !ready_) {
            ready_ = request_->is_complete();
            if (ready_) {
                value_ = extract_value();
            }
        }
        return ready_; 
    }

    /// Block until the result is available.
    T get() { 
        if (!ready_) {
            if (request_) {
                request_->wait();
            }
            value_ = extract_value();
            ready_ = true;
        }
        return value_; 
    }

    /// Block with timeout.  Returns std::nullopt on timeout.
    std::optional<T> get(std::chrono::milliseconds timeout) { 
        if (!ready_) {
            if (request_) {
                auto st = request_->wait(timeout);
                if (!st.ok() && st.code() != ErrorCode::kInProgress) {
                    return std::nullopt;
                }
            }
            if (ready_ || (request_ && request_->is_complete())) {
                value_ = extract_value();
                ready_ = true;
            } else {
                return std::nullopt;
            }
        }
        return value_; 
    }

    /// Current status.
    [[nodiscard]] Status status() const noexcept { 
        if (request_) {
            return request_->status();
        }
        return status_; 
    }

    /// Cancel the underlying operation.
    void cancel() { 
        if (request_) {
            request_->cancel();
        }
    }

    /// Chain a continuation: f.then([](T val) { ... }) -> Future<U>.
    template <typename Func>
    auto then(Func&&) -> Future<std::invoke_result_t<Func, T>> {
        return Future<std::invoke_result_t<Func, T>>::make_error(
            Status(ErrorCode::kNotImplemented, "then() not implemented"));
    }

    /// Attach a callback invoked on completion (on the progress thread).
    void on_complete(std::function<void(Status, T)>) {
    }

    /// Access the underlying Request (advanced use).
    [[nodiscard]] Request::Ptr request() const noexcept { return request_; }

    // --- Factory methods ------------------------------------------------------

    /// Create a future that's already completed with a value.
    static Future make_ready(T value) { return Future(value); }

    /// Create a future that's already completed with an error.
    static Future make_error(Status status) {
        Future f;
        f.status_ = status;
        f.ready_ = true;
        return f;
    }

    static Future make_request(Request::Ptr req) {
        Future f;
        f.request_ = std::move(req);
        f.ready_ = false;
        return f;
    }

private:
    friend class Worker;
    friend class Endpoint;
    explicit Future(Request::Ptr req) : request_(std::move(req)), ready_(false) {}
    explicit Future(T value) : value_(std::move(value)), ready_(true) {}

    T extract_value() const {
        if constexpr (std::is_same_v<T, std::pair<size_t, Tag>>) {
            if (request_) {
                return std::make_pair(request_->bytes_transferred(), request_->matched_tag());
            }
        } else if constexpr (std::is_same_v<T, size_t>) {
            if (request_) {
                return request_->bytes_transferred();
            }
        }
        return value_;
    }

    Request::Ptr request_;
    mutable T value_{};
    Status status_{Status::OK()};
    mutable bool ready_ = true;
};

// Specialization for void
template<>
class Future<void> {
public:
    Future() = default;
    ~Future() = default;

    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;
    Future(Future&&) noexcept = default;
    Future& operator=(Future&&) = default;

    [[nodiscard]] bool ready() const noexcept { 
        if (request_ && !ready_) {
            ready_ = request_->is_complete();
        }
        return ready_; 
    }
    void get() {
        if (!ready_ && request_) {
            request_->wait();
            ready_ = true;
        }
    }
    std::optional<std::nullptr_t> get(std::chrono::milliseconds timeout) { 
        if (!ready_) {
            if (request_) {
                auto st = request_->wait(timeout);
                if (!st.ok() && st.code() != ErrorCode::kInProgress) {
                    return std::nullopt;
                }
            }
            if (ready_ || (request_ && request_->is_complete())) {
                ready_ = true;
            } else {
                return std::nullopt;
            }
        }
        return nullptr; 
    }
    [[nodiscard]] Status status() const noexcept { 
        if (request_) {
            return request_->status();
        }
        return impl_ ? impl_->status_ : Status::OK(); 
    }
    void cancel() {
        if (request_) {
            request_->cancel();
        }
    }
    [[nodiscard]] Request::Ptr request() const noexcept { return request_; }

    static Future make_ready() { return Future{}; }
    static Future make_error(Status status) {
        Future f;
        if (!f.impl_) {
            f.impl_ = std::make_unique<Impl>();
        }
        f.impl_->status_ = status;
        f.ready_ = true;
        return f;
    }

    static Future make_request(Request::Ptr req) {
        Future f;
        f.request_ = std::move(req);
        f.ready_ = false;
        return f;
    }

private:
    friend class Worker;
    friend class Endpoint;
    explicit Future(Request::Ptr req) : request_(std::move(req)), ready_(false) {}
    struct Impl {
        Status status_{Status::OK()};
        bool ready_ = true;
    };
    std::unique_ptr<Impl> impl_;
    Request::Ptr request_;
    mutable bool ready_ = true;
};

// Forward declare Endpoint (included via p2p/endpoint.h in user code)
// The template will be instantiated for std::shared_ptr<Endpoint>

// ---------------------------------------------------------------------------
// wait_all / wait_any – batch utilities
// ---------------------------------------------------------------------------

/// Wait for all futures to complete.  Returns the first error status, or OK.
template <typename T>
Status wait_all(std::vector<Future<T>>& futures,
                std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});

/// Wait for at least one future to complete.  Returns the index.
template <typename T>
std::optional<size_t> wait_any(std::vector<Future<T>>& futures,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});

}  // namespace p2p
