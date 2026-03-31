#include "zerokv/common.h"

#include <ucp/api/ucp.h>

#include <array>
#include <unordered_map>

namespace {

struct AXONErrorCategory : std::error_category {
    const char* name() const noexcept override {
        return "axon";
    }

    std::string message(int ev) const override {
        switch (static_cast<axon::ErrorCode>(ev)) {
            case axon::ErrorCode::kSuccess: return "Success";
            case axon::ErrorCode::kInProgress: return "Operation in progress";
            case axon::ErrorCode::kCanceled: return "Operation canceled";
            case axon::ErrorCode::kTimeout: return "Operation timed out";
            case axon::ErrorCode::kConnectionRefused: return "Connection refused";
            case axon::ErrorCode::kConnectionReset: return "Connection reset";
            case axon::ErrorCode::kEndpointClosed: return "Endpoint closed";
            case axon::ErrorCode::kTransportError: return "Transport error";
            case axon::ErrorCode::kMessageTruncated: return "Message truncated";
            case axon::ErrorCode::kTagMismatch: return "Tag mismatch";
            case axon::ErrorCode::kOutOfMemory: return "Out of memory";
            case axon::ErrorCode::kInvalidBuffer: return "Invalid buffer";
            case axon::ErrorCode::kRegistrationFailed: return "Memory registration failed";
            case axon::ErrorCode::kPluginNotFound: return "Plugin not found";
            case axon::ErrorCode::kPluginInitFailed: return "Plugin initialization failed";
            case axon::ErrorCode::kInvalidArgument: return "Invalid argument";
            case axon::ErrorCode::kNotImplemented: return "Not implemented";
            case axon::ErrorCode::kInternalError: return "Internal error";
            default: return "Unknown error";
        }
    }
};

const AXONErrorCategory& get_axon_category() {
    static AXONErrorCategory instance;
    return instance;
}

// Map UCX status to AXON error code
axon::ErrorCode ucx_to_axon_error(ucs_status_t s) {
    switch (s) {
        case UCS_OK:
            return axon::ErrorCode::kSuccess;
        case UCS_INPROGRESS:
            return axon::ErrorCode::kInProgress;
        case UCS_ERR_CANCELED:
            return axon::ErrorCode::kCanceled;
        case UCS_ERR_NO_RESOURCE:
        case UCS_ERR_NO_MEMORY:
            return axon::ErrorCode::kOutOfMemory;
        case UCS_ERR_NO_ELEM:
            return axon::ErrorCode::kOutOfMemory;
        case UCS_ERR_INVALID_PARAM:
            return axon::ErrorCode::kInvalidArgument;
        case UCS_ERR_NO_MESSAGE:
            return axon::ErrorCode::kConnectionReset;
        case UCS_ERR_CONNECTION_RESET:
            return axon::ErrorCode::kConnectionReset;
        case UCS_ERR_NOT_CONNECTED:
            return axon::ErrorCode::kConnectionRefused;
        case UCS_ERR_MESSAGE_TRUNCATED:
            return axon::ErrorCode::kMessageTruncated;
        case UCS_ERR_EXCEEDS_LIMIT:
            return axon::ErrorCode::kOutOfMemory;
        default:
            return axon::ErrorCode::kTransportError;
    }
}

} // anonymous namespace

namespace axon {

const std::error_category& axon_category() noexcept {
    return get_axon_category();
}

// Convert UCX status to AXON Status
Status from_ucs_status(ucs_status_t s, std::string msg = {}) {
    if (s == UCS_OK) {
        return Status::OK();
    }
    return Status(ucx_to_axon_error(s), std::move(msg));
}

} // namespace axon
