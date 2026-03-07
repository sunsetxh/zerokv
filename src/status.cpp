#include "p2p/common.h"

#include <ucp/api/ucp.h>

#include <array>
#include <unordered_map>

namespace {

struct P2PErrorCategory : std::error_category {
    const char* name() const noexcept override {
        return "p2p";
    }

    std::string message(int ev) const override {
        switch (static_cast<p2p::ErrorCode>(ev)) {
            case p2p::ErrorCode::kSuccess: return "Success";
            case p2p::ErrorCode::kInProgress: return "Operation in progress";
            case p2p::ErrorCode::kCanceled: return "Operation canceled";
            case p2p::ErrorCode::kTimeout: return "Operation timed out";
            case p2p::ErrorCode::kConnectionRefused: return "Connection refused";
            case p2p::ErrorCode::kConnectionReset: return "Connection reset";
            case p2p::ErrorCode::kEndpointClosed: return "Endpoint closed";
            case p2p::ErrorCode::kTransportError: return "Transport error";
            case p2p::ErrorCode::kMessageTruncated: return "Message truncated";
            case p2p::ErrorCode::kTagMismatch: return "Tag mismatch";
            case p2p::ErrorCode::kOutOfMemory: return "Out of memory";
            case p2p::ErrorCode::kInvalidBuffer: return "Invalid buffer";
            case p2p::ErrorCode::kRegistrationFailed: return "Memory registration failed";
            case p2p::ErrorCode::kPluginNotFound: return "Plugin not found";
            case p2p::ErrorCode::kPluginInitFailed: return "Plugin initialization failed";
            case p2p::ErrorCode::kInvalidArgument: return "Invalid argument";
            case p2p::ErrorCode::kNotImplemented: return "Not implemented";
            case p2p::ErrorCode::kInternalError: return "Internal error";
            default: return "Unknown error";
        }
    }
};

const P2PErrorCategory& get_p2p_category() {
    static P2PErrorCategory instance;
    return instance;
}

// Map UCX status to P2P error code
p2p::ErrorCode ucx_to_p2p_error(ucs_status_t s) {
    switch (s) {
        case UCS_OK:
            return p2p::ErrorCode::kSuccess;
        case UCS_INPROGRESS:
            return p2p::ErrorCode::kInProgress;
        case UCS_ERR_CANCELED:
            return p2p::ErrorCode::kCanceled;
        case UCS_ERR_NO_RESOURCE:
        case UCS_ERR_NO_MEMORY:
            return p2p::ErrorCode::kOutOfMemory;
        case UCS_ERR_NO_ELEM:
            return p2p::ErrorCode::kOutOfMemory;
        case UCS_ERR_INVALID_PARAM:
            return p2p::ErrorCode::kInvalidArgument;
        case UCS_ERR_NO_MESSAGE:
            return p2p::ErrorCode::kConnectionReset;
        case UCS_ERR_CONNECTION_RESET:
            return p2p::ErrorCode::kConnectionReset;
        case UCS_ERR_NOT_CONNECTED:
            return p2p::ErrorCode::kConnectionRefused;
        case UCS_ERR_MESSAGE_TRUNCATED:
            return p2p::ErrorCode::kMessageTruncated;
        case UCS_ERR_EXCEEDS_LIMIT:
            return p2p::ErrorCode::kOutOfMemory;
        default:
            return p2p::ErrorCode::kTransportError;
    }
}

} // anonymous namespace

namespace p2p {

const std::error_category& p2p_category() noexcept {
    return get_p2p_category();
}

// Convert UCX status to P2P Status
Status from_ucs_status(ucs_status_t s, std::string msg = {}) {
    if (s == UCS_OK) {
        return Status::OK();
    }
    return Status(ucx_to_p2p_error(s), std::move(msg));
}

} // namespace p2p
