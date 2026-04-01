#pragma once

/// @file zerokv/common.h
/// @brief Common types, status codes, and forward declarations for the ZeroKV library.

#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>

namespace zerokv {

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------
constexpr int kVersionMajor = 0;
constexpr int kVersionMinor = 1;
constexpr int kVersionPatch = 0;

// ---------------------------------------------------------------------------
// Status / Error handling
// ---------------------------------------------------------------------------

/// Fine-grained error codes exposed through std::error_code.
enum class ErrorCode : int {
    kSuccess             = 0,
    kInProgress          = 1,   // async operation still pending
    kCanceled            = 2,
    kTimeout             = 3,

    // Connection errors 100-199
    kConnectionRefused   = 100,
    kConnectionReset     = 101,
    kEndpointClosed      = 102,

    // Transport errors 200-299
    kTransportError      = 200,
    kMessageTruncated    = 201,
    kTagMismatch         = 202,

    // Memory errors 300-399
    kOutOfMemory         = 300,
    kInvalidBuffer       = 301,
    kRegistrationFailed  = 302,

    // Plugin errors 400-499
    kPluginNotFound      = 400,
    kPluginInitFailed    = 401,

    // Generic
    kInvalidArgument     = 900,
    kNotImplemented     = 901,
    kInternalError       = 999,
};

/// Category singleton for std::error_code integration.
const std::error_category& zerokv_category() noexcept;

inline std::error_code make_error_code(ErrorCode ec) noexcept {
    return {static_cast<int>(ec), zerokv_category()};
}

/// Lightweight status wrapper – implicitly convertible from ErrorCode.
class Status {
public:
    Status() noexcept : code_(ErrorCode::kSuccess) {}
    Status(ErrorCode code) noexcept : code_(code) {}          // NOLINT
    Status(ErrorCode code, std::string msg) noexcept
        : code_(code), message_(std::move(msg)) {}

    [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::kSuccess; }
    [[nodiscard]] bool in_progress() const noexcept { return code_ == ErrorCode::kInProgress; }
    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    [[nodiscard]] std::error_code error_code() const noexcept {
        return make_error_code(code_);
    }

    /// Throw std::system_error if !ok().
    void throw_if_error() const {
        if (!ok() && !in_progress()) {
            throw std::system_error(error_code(), message_);
        }
    }

    static Status OK() noexcept { return {}; }

private:
    ErrorCode   code_;
    std::string message_;
};

// ---------------------------------------------------------------------------
// Tag matching
// ---------------------------------------------------------------------------

/// A tag is a 64-bit value used for message matching on the receiver side.
/// The upper 32 bits are typically used as a communicator/context ID,
/// the lower 32 bits as a user-defined message tag.
using Tag = uint64_t;

/// Wildcard: matches any tag.
constexpr Tag kTagAny = ~Tag{0};

/// Helper to compose a tag from (context_id, user_tag).
constexpr Tag make_tag(uint32_t context_id, uint32_t user_tag) noexcept {
    return (static_cast<Tag>(context_id) << 32) | user_tag;
}
constexpr uint32_t tag_context(Tag t) noexcept { return static_cast<uint32_t>(t >> 32); }
constexpr uint32_t tag_user(Tag t) noexcept    { return static_cast<uint32_t>(t); }

/// Tag mask for selective matching.
constexpr Tag kTagMaskAll  = ~Tag{0};
constexpr Tag kTagMaskUser = 0x0000'0000'FFFF'FFFF;

// ---------------------------------------------------------------------------
// Memory types
// ---------------------------------------------------------------------------

enum class MemoryType : uint8_t {
    kHost   = 0,   // CPU / system memory
    kCuda   = 1,   // NVIDIA GPU
    kRocm   = 2,   // AMD GPU
    kAscend = 3,   // Huawei Ascend NPU
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

class Config;
class Context;
class Worker;
class Endpoint;
class Cluster;
class MemoryRegion;
class MemoryPool;
class Request;

template <typename T> class Future;
template <typename T> class Promise;

}  // namespace zerokv

// Enable std::error_code interop.
namespace std {
template<> struct is_error_code_enum<zerokv::ErrorCode> : true_type {};
}
