#pragma once

/// @file zerokv/transport/future.h
/// @brief Asynchronous operation primitives: Request, Future<T>, callback support.
///
/// Design rationale:
///   - Request is the low-level handle representing one in-flight UCX operation.
///   - Future<T> wraps Request with a typed, composable async result.
///   - Callbacks are optional; users may poll, wait, or compose futures.
///   - Inspired by std::future but non-blocking by default.

#include "zerokv/common.h"

#include <ucp/api/ucp.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <variant>

namespace zerokv::transport {

using zerokv::ErrorCode;
using zerokv::Status;
using zerokv::Tag;

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
    [[nodiscard]] zerokv::Status status() const noexcept;

    /// Block until complete or timeout.
    /// Returns the final zerokv::Status.
    zerokv::Status wait(std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});

    /// Attempt to cancel the operation.
    /// Cancellation is best-effort; the completion callback will still fire
    /// with status kCanceled.
    void cancel();

    /// Number of bytes actually transferred (valid after completion).
    [[nodiscard]] size_t bytes_transferred() const noexcept;

    /// The tag that was matched (valid after recv completion).
    [[nodiscard]] zerokv::Tag matched_tag() const noexcept;

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
                      std::shared_ptr<zerokv::Tag> async_matched_tag,
                      std::shared_ptr<void> keep_alive = {});

    /// Atomic operation result (valid after atomic completion).
    /// Reads from async_bytes_transferred when is_atomic_ is true.
    [[nodiscard]] uint64_t atomic_result() const noexcept;

private:
    friend class Worker;
    friend class Endpoint;
    Request() = default;
    void populate_recv_info() const;
    struct Impl {
        void* ucx_request_ = nullptr;
        ucp_worker_h worker_ = nullptr;
        zerokv::Status status_;
        mutable size_t bytes_transferred_ = 0;
        mutable zerokv::Tag matched_tag_ = 0;
        std::shared_ptr<size_t> async_bytes_transferred_;
        std::shared_ptr<zerokv::Tag> async_matched_tag_;
        bool is_atomic_ = false;  // When true, async_bytes_transferred_ holds atomic result
        std::shared_ptr<void> keep_alive_;  // Keeps MemoryRegion alive for async ops
    };
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Completion callback
// ---------------------------------------------------------------------------

/// Signature for completion callbacks.  Receives the final zerokv::Status and the
/// number of bytes transferred.
using CompletionCallback = std::function<void(zerokv::Status status, size_t bytes)>;

// ---------------------------------------------------------------------------
// Future<T> – typed async result
// ---------------------------------------------------------------------------

/// A non-blocking future that wraps an async operation.
///
/// @tparam T  The result type.  `void` for fire-and-forget sends.
///            `size_t` for recv (bytes received).
///            `std::pair<size_t, zerokv::Tag>` for tag_recv (bytes + matched tag).
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
                if (!st.ok() && st.code() != zerokv::ErrorCode::kInProgress) {
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
    [[nodiscard]] zerokv::Status status() const noexcept { 
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
            zerokv::Status(zerokv::ErrorCode::kNotImplemented, "then() not implemented"));
    }

    /// Attach a callback invoked on completion (on the progress thread).
    void on_complete(std::function<void(zerokv::Status, T)>) {
    }

    /// Access the underlying Request (advanced use).
    [[nodiscard]] Request::Ptr request() const noexcept { return request_; }

    // --- Factory methods ------------------------------------------------------

    /// Create a future that's already completed with a value.
    static Future make_ready(T value) { return Future(value); }

    /// Create a future that's already completed with an error.
    static Future make_error(zerokv::Status status) {
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
        if constexpr (std::is_same_v<T, std::pair<size_t, zerokv::Tag>>) {
            if (request_) {
                return std::make_pair(request_->bytes_transferred(), request_->matched_tag());
            }
        } else if constexpr (std::is_same_v<T, size_t>) {
            if (request_) {
                return request_->bytes_transferred();
            }
        } else if constexpr (std::is_same_v<T, uint64_t>) {
            if (request_) {
                return request_->atomic_result();
            }
        }
        return value_;
    }

    Request::Ptr request_;
    mutable T value_{};
    zerokv::Status status_{zerokv::Status::OK()};
    mutable bool ready_ = true;
};

// Specialization for void
template<>
class Future<void> {
public:
    struct SharedState {
        mutable std::mutex mu;
        std::condition_variable cv;
        zerokv::Status status_{zerokv::Status(ErrorCode::kInProgress)};
        bool ready_ = false;
    };

    Future() = default;
    ~Future() = default;

    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;
    Future(Future&&) noexcept = default;
    Future& operator=(Future&&) = default;

    [[nodiscard]] bool ready() const noexcept { 
        if (shared_state_) {
            std::lock_guard<std::mutex> lock(shared_state_->mu);
            return shared_state_->ready_;
        }
        if (request_ && !ready_) {
            ready_ = request_->is_complete();
        }
        return ready_; 
    }
    void get() {
        if (shared_state_) {
            std::unique_lock<std::mutex> lock(shared_state_->mu);
            shared_state_->cv.wait(lock, [&] { return shared_state_->ready_; });
            ready_ = true;
            return;
        }
        if (!ready_ && request_) {
            request_->wait();
            ready_ = true;
        }
    }
    std::optional<std::nullptr_t> get(std::chrono::milliseconds timeout) { 
        if (shared_state_) {
            std::unique_lock<std::mutex> lock(shared_state_->mu);
            if (!shared_state_->cv.wait_for(lock, timeout, [&] { return shared_state_->ready_; })) {
                return std::nullopt;
            }
            ready_ = true;
            return nullptr;
        }
        if (!ready_) {
            if (request_) {
                auto st = request_->wait(timeout);
                if (!st.ok() && st.code() != zerokv::ErrorCode::kInProgress) {
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
    [[nodiscard]] zerokv::Status status() const noexcept { 
        if (shared_state_) {
            std::lock_guard<std::mutex> lock(shared_state_->mu);
            return shared_state_->ready_ ? shared_state_->status_
                                         : zerokv::Status(ErrorCode::kInProgress);
        }
        if (request_) {
            return request_->status();
        }
        return impl_ ? impl_->status_ : zerokv::Status::OK(); 
    }
    void cancel() {
        if (request_) {
            request_->cancel();
        }
    }
    [[nodiscard]] Request::Ptr request() const noexcept { return request_; }

    static Future make_ready() { return Future{}; }
    static Future make_error(zerokv::Status status) {
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

    static Future make_shared_state(const std::shared_ptr<SharedState>& state) {
        Future f;
        f.shared_state_ = state;
        f.ready_ = false;
        return f;
    }

private:
    friend class Worker;
    friend class Endpoint;
    friend class Promise<void>;
    explicit Future(Request::Ptr req) : request_(std::move(req)), ready_(false) {}
    struct Impl {
        zerokv::Status status_{zerokv::Status::OK()};
        bool ready_ = true;
    };
    std::unique_ptr<Impl> impl_;
    Request::Ptr request_;
    std::shared_ptr<SharedState> shared_state_;
    mutable bool ready_ = true;
};

template<>
class Promise<void> {
public:
    Promise() : state_(std::make_shared<Future<void>::SharedState>()) {}

    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;
    Promise(Promise&&) noexcept = default;
    Promise& operator=(Promise&&) noexcept = default;

    [[nodiscard]] Future<void> get_future() const {
        return Future<void>::make_shared_state(state_);
    }

    void set_value() {
        std::lock_guard<std::mutex> lock(state_->mu);
        state_->status_ = zerokv::Status::OK();
        state_->ready_ = true;
        state_->cv.notify_all();
    }

    void set_error(zerokv::Status status) {
        std::lock_guard<std::mutex> lock(state_->mu);
        state_->status_ = std::move(status);
        state_->ready_ = true;
        state_->cv.notify_all();
    }

private:
    std::shared_ptr<Future<void>::SharedState> state_;
};

// Forward declare Endpoint (included via zerokv/endpoint.h in user code)
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

}  // namespace zerokv::transport
