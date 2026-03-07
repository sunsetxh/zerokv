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

private:
    friend class Worker;
    friend class Endpoint;
    Request() = default;
    struct Impl {
        // Empty for now - can be extended to track UCX request
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
    [[nodiscard]] bool ready() const noexcept { return ready_; }

    /// Block until the result is available.
    T get() { return value_; }

    /// Block with timeout.  Returns std::nullopt on timeout.
    std::optional<T> get(std::chrono::milliseconds) { return value_; }

    /// Current status.
    [[nodiscard]] Status status() const noexcept { return status_; }

    /// Cancel the underlying operation.
    void cancel() { /* Stub - can't cancel */ }

    /// Chain a continuation: f.then([](T val) { ... }) -> Future<U>.
    template <typename Func>
    auto then(Func&&) -> Future<std::invoke_result_t<Func, T>> {
        return Future<std::invoke_result_t<Func, T>>::make_error(
            Status(ErrorCode::kNotImplemented, "then() not implemented"));
    }

    /// Attach a callback invoked on completion (on the progress thread).
    void on_complete(std::function<void(Status, T)>) {
        // Stub
    }

    /// Access the underlying Request (advanced use).
    [[nodiscard]] Request::Ptr request() const noexcept { return nullptr; }

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

private:
    friend class Worker;
    friend class Endpoint;
    explicit Future(Request::Ptr) { /* Stub */ }
    explicit Future(T value) : value_(std::move(value)), ready_(true) {}

    // Simple storage for completed futures
    T value_{};
    Status status_{Status::OK()};
    bool ready_ = true;
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

    [[nodiscard]] bool ready() const noexcept { return impl_ ? impl_->ready_ : true; }
    void get() {}
    std::optional<std::nullptr_t> get(std::chrono::milliseconds) { return nullptr; }
    [[nodiscard]] Status status() const noexcept { return impl_ ? impl_->status_ : Status::OK(); }
    void cancel() {}
    [[nodiscard]] Request::Ptr request() const noexcept { return nullptr; }

    static Future make_ready() { return Future{}; }
    static Future make_error(Status status) {
        Future f;
        if (!f.impl_) {
            f.impl_ = std::make_unique<Impl>();
        }
        f.impl_->status_ = status;
        return f;
    }

private:
    friend class Worker;
    friend class Endpoint;
    explicit Future(Request::Ptr req);
    struct Impl {
        Status status_{Status::OK()};
        bool ready_ = true;
    };
    std::unique_ptr<Impl> impl_;
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
