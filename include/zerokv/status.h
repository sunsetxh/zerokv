/**
 * Copyright (c) 2025. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ZEROKV_STATUS_H
#define ZEROKV_STATUS_H

#include <string>
#include <memory>

namespace zerokv {

/**
 * @brief Status codes for ZeroKV operations
 */
enum class StatusCode {
    OK = 0,                 // Success
    INVALID_ARGUMENT,       // Invalid argument
    NOT_FOUND,              // Key not found
    ALREADY_EXISTS,         // Key already exists
    RESOURCE_EXHAUSTED,     // Resource exhausted (e.g., max connections)
    UNAVAILABLE,            // Service unavailable
    DEADLINE_EXCEEDED,      // Operation timeout
    INTERNAL,               // Internal error
    UNIMPLEMENTED,          // Feature not implemented
    UNKNOWN                 // Unknown error
};

/**
 * @brief Status class for error handling
 *
 * This class represents the result of an operation.
 * Use Status::OK() for success, and Status::Error() for failures.
 */
class Status {
public:
    /**
     * @brief Create a success status
     */
    Status() : code_(StatusCode::OK) {}

    /**
     * @brief Create a status with code and message
     */
    Status(StatusCode code, const std::string& message)
        : code_(code), message_(std::make_shared<std::string>(message)) {}

    /**
     * @brief Copy constructor
     */
    Status(const Status& other) = default;

    /**
     * @brief Assignment operator
     */
    Status& operator=(const Status& other) = default;

    /**
     * @brief Check if status is OK
     */
    bool ok() const {
        return code_ == StatusCode::OK;
    }

    /**
     * @brief Get status code
     */
    StatusCode code() const {
        return code_;
    }

    /**
     * @brief Get error message
     */
    std::string message() const {
        return message_ ? *message_ : "";
    }

    /**
     * @brief Get string representation
     */
    std::string ToString() const {
        if (ok()) {
            return "OK";
        }
        return StatusCodeToString(code_) + ": " + message();
    }

    /**
     * @brief Create a success status
     */
    static Status OK() {
        return Status();
    }

    /**
     * @brief Create an error status
     */
    static Status Error(const std::string& message) {
        return Status(StatusCode::INTERNAL, message);
    }

    /**
     * @brief Create an error status with specific code
     */
    static Status Error(StatusCode code, const std::string& message) {
        return Status(code, message);
    }

    /**
     * @brief Create an invalid argument status
     */
    static Status InvalidArgument(const std::string& message) {
        return Status(StatusCode::INVALID_ARGUMENT, message);
    }

    /**
     * @brief Create a not found status
     */
    static Status NotFound(const std::string& message) {
        return Status(StatusCode::NOT_FOUND, message);
    }

    /**
     * @brief Create an already exists status
     */
    static Status AlreadyExists(const std::string& message) {
        return Status(StatusCode::ALREADY_EXISTS, message);
    }

    /**
     * @brief Create a resource exhausted status
     */
    static Status ResourceExhausted(const std::string& message) {
        return Status(StatusCode::RESOURCE_EXHAUSTED, message);
    }

    /**
     * @brief Create an unavailable status
     */
    static Status Unavailable(const std::string& message) {
        return Status(StatusCode::UNAVAILABLE, message);
    }

    /**
     * @brief Create a deadline exceeded status
     */
    static Status DeadlineExceeded(const std::string& message) {
        return Status(StatusCode::DEADLINE_EXCEEDED, message);
    }

private:
    StatusCode code_;
    std::shared_ptr<std::string> message_;

    static const char* StatusCodeToString(StatusCode code) {
        switch (code) {
            case StatusCode::OK:                 return "OK";
            case StatusCode::INVALID_ARGUMENT:   return "INVALID_ARGUMENT";
            case StatusCode::NOT_FOUND:          return "NOT_FOUND";
            case StatusCode::ALREADY_EXISTS:     return "ALREADY_EXISTS";
            case StatusCode::RESOURCE_EXHAUSTED: return "RESOURCE_EXHAUSTED";
            case StatusCode::UNAVAILABLE:        return "UNAVAILABLE";
            case StatusCode::DEADLINE_EXCEEDED:  return "DEADLINE_EXCEEDED";
            case StatusCode::INTERNAL:           return "INTERNAL";
            case StatusCode::UNIMPLEMENTED:      return "UNIMPLEMENTED";
            case StatusCode::UNKNOWN:            return "UNKNOWN";
            default:                             return "UNKNOWN";
        }
    }
};

}  // namespace zerokv

#endif  // ZEROKV_STATUS_H
