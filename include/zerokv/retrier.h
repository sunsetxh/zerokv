// Error handling and retry logic
#pragma once

#include <functional>
#include <chrono>
#include <thread>
#include <atomic>
#include <variant>
#include <optional>
#include <string>

namespace zerokv {

// Error codes
enum class ErrorCode {
    OK = 0,
    NETWORK_ERROR,
    TIMEOUT,
    NOT_FOUND,
    OUT_OF_MEMORY,
    INVALID_ARGUMENT,
    INTERNAL_ERROR,
    CONNECTION_FAILED,
    OPERATION_FAILED
};

// Error with context
struct Error {
    ErrorCode code;
    std::string message;
    std::string details;

    Error(ErrorCode c, const std::string& msg)
        : code(c), message(msg) {}

    Error(ErrorCode c, const std::string& msg, const std::string& det)
        : code(c), message(msg), details(det) {}

    bool ok() const { return code == ErrorCode::OK; }
    std::string to_string() const;
};

// Retry configuration
struct RetryConfig {
    int max_attempts = 3;
    int initial_delay_ms = 100;
    int max_delay_ms = 5000;
    double backoff_multiplier = 2.0;
    bool use_jitter = true;
};

// Retry policy
class RetryPolicy {
public:
    explicit RetryPolicy(const RetryConfig& config = {});

    // Check if should retry
    bool should_retry(int attempt, const Error& error) const;

    // Get delay before next retry
    int get_delay_ms(int attempt) const;

    // Execute with retry
    template<typename Func, typename... Args>
    auto execute(Func func, Args&&... args)
        -> std::variant<Error, std::result_of_t<Func(Args...)>>;

private:
    RetryConfig config_;
    std::atomic<int> attempt_{0};

    int calculate_delay(int attempt) const;
    int random_jitter(int delay) const;
};

// Result type (similar to Rust Result)
template<typename T>
class Result {
public:
    Result(T&& value) : value_(std::move(value)), error_(std::nullopt) {}
    Result(Error&& error) : value_(std::nullopt), error_(std::move(error)) {}

    bool ok() const { return error_.has_value() == false; }
    const T& value() const { return *value_; }
    const Error& error() const { return *error_; }

    T& value() { return *value_; }

    // Map over the value
    template<typename F>
    auto map(F&& func) -> Result<decltype(func(std::declval<T>()))> {
        if (ok()) {
            return Result<decltype(func(std::declval<T>()))>(func(value()));
        }
        return Result<decltype(func(std::declval<T>()))>(Error(*error_));
    }

private:
    std::optional<T> value_;
    std::optional<Error> error_;
};

// Specialization for void
template<>
class Result<void> {
public:
    Result() : error_(std::nullopt) {}
    Result(Error&& error) : error_(std::move(error)) {}

    bool ok() const { return error_.has_value() == false; }
    const Error& error() const { return *error_; }

private:
    std::optional<Error> error_;
};

// Execute with retry helper
template<typename Func, typename... Args>
auto RetryPolicy::execute(Func func, Args&&... args)
    -> std::variant<Error, std::result_of_t<Func(Args...)>> {

    for (int attempt = 0; attempt < config_.max_attempts; ++attempt) {
        attempt_.store(attempt);

        auto result = func(std::forward<Args>(args)...);

        if (result.ok()) {
            return result.value();
        }

        if (!should_retry(attempt, result.error())) {
            return result.error();
        }

        int delay = get_delay_ms(attempt);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }

    return Error(ErrorCode::OPERATION_FAILED, "Max retries exceeded");
}

} // namespace zerokv
