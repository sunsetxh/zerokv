#include "zerokv/retrier.h"
#include <random>
#include <sstream>

namespace zerokv {

std::string Error::to_string() const {
    std::ostringstream oss;
    oss << "Error(";
    switch (code) {
        case ErrorCode::OK: oss << "OK"; break;
        case ErrorCode::NETWORK_ERROR: oss << "NETWORK_ERROR"; break;
        case ErrorCode::TIMEOUT: oss << "TIMEOUT"; break;
        case ErrorCode::NOT_FOUND: oss << "NOT_FOUND"; break;
        case ErrorCode::OUT_OF_MEMORY: oss << "OUT_OF_MEMORY"; break;
        case ErrorCode::INVALID_ARGUMENT: oss << "INVALID_ARGUMENT"; break;
        case ErrorCode::INTERNAL_ERROR: oss << "INTERNAL_ERROR"; break;
        case ErrorCode::CONNECTION_FAILED: oss << "CONNECTION_FAILED"; break;
        case ErrorCode::OPERATION_FAILED: oss << "OPERATION_FAILED"; break;
    }
    oss << "): " << message;
    if (!details.empty()) {
        oss << " (" << details << ")";
    }
    return oss.str();
}

RetryPolicy::RetryPolicy(const RetryConfig& config) : config_(config) {}

bool RetryPolicy::should_retry(int attempt, const Error& error) const {
    if (attempt >= config_.max_attempts) {
        return false;
    }

    // Don't retry on these errors
    switch (error.code) {
        case ErrorCode::OK:
        case ErrorCode::INVALID_ARGUMENT:
        case ErrorCode::NOT_FOUND:
            return false;
        default:
            break;
    }

    return true;
}

int RetryPolicy::get_delay_ms(int attempt) const {
    int delay = calculate_delay(attempt);

    if (config_.use_jitter) {
        delay = random_jitter(delay);
    }

    return delay;
}

int RetryPolicy::calculate_delay(int attempt) const {
    int delay = config_.initial_delay_ms;

    for (int i = 0; i < attempt; i++) {
        delay = static_cast<int>(delay * config_.backoff_multiplier);
        if (delay > config_.max_delay_ms) {
            delay = config_.max_delay_ms;
            break;
        }
    }

    return delay;
}

int RetryPolicy::random_jitter(int delay) const {
    static thread_local std::mt19937 rng(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::uniform_int_distribution<> dist(-delay / 4, delay / 4);
    return delay + dist(rng);
}

} // namespace zerokv
