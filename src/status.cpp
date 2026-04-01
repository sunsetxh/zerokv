#include "zerokv/common.h"

#include <ucp/api/ucp.h>

#include <array>
#include <unordered_map>

namespace {

struct ZeroKVErrorCategory : std::error_category {
    const char* name() const noexcept override {
        return "zerokv";
    }

    std::string message(int ev) const override {
        switch (static_cast<zerokv::ErrorCode>(ev)) {
            case zerokv::ErrorCode::kSuccess: return "Success";
            case zerokv::ErrorCode::kInProgress: return "Operation in progress";
            case zerokv::ErrorCode::kCanceled: return "Operation canceled";
            case zerokv::ErrorCode::kTimeout: return "Operation timed out";
            case zerokv::ErrorCode::kConnectionRefused: return "Connection refused";
            case zerokv::ErrorCode::kConnectionReset: return "Connection reset";
            case zerokv::ErrorCode::kEndpointClosed: return "Endpoint closed";
            case zerokv::ErrorCode::kTransportError: return "Transport error";
            case zerokv::ErrorCode::kMessageTruncated: return "Message truncated";
            case zerokv::ErrorCode::kTagMismatch: return "Tag mismatch";
            case zerokv::ErrorCode::kOutOfMemory: return "Out of memory";
            case zerokv::ErrorCode::kInvalidBuffer: return "Invalid buffer";
            case zerokv::ErrorCode::kRegistrationFailed: return "Memory registration failed";
            case zerokv::ErrorCode::kPluginNotFound: return "Plugin not found";
            case zerokv::ErrorCode::kPluginInitFailed: return "Plugin initialization failed";
            case zerokv::ErrorCode::kInvalidArgument: return "Invalid argument";
            case zerokv::ErrorCode::kNotImplemented: return "Not implemented";
            case zerokv::ErrorCode::kInternalError: return "Internal error";
            default: return "Unknown error";
        }
    }
};

const ZeroKVErrorCategory& get_zerokv_category() {
    static ZeroKVErrorCategory instance;
    return instance;
}

// Map UCX status to zerokv error code
zerokv::ErrorCode ucx_to_zerokv_error(ucs_status_t s) {
    switch (s) {
        case UCS_OK:
            return zerokv::ErrorCode::kSuccess;
        case UCS_INPROGRESS:
            return zerokv::ErrorCode::kInProgress;
        case UCS_ERR_CANCELED:
            return zerokv::ErrorCode::kCanceled;
        case UCS_ERR_NO_RESOURCE:
        case UCS_ERR_NO_MEMORY:
            return zerokv::ErrorCode::kOutOfMemory;
        case UCS_ERR_NO_ELEM:
            return zerokv::ErrorCode::kOutOfMemory;
        case UCS_ERR_INVALID_PARAM:
            return zerokv::ErrorCode::kInvalidArgument;
        case UCS_ERR_NO_MESSAGE:
            return zerokv::ErrorCode::kConnectionReset;
        case UCS_ERR_CONNECTION_RESET:
            return zerokv::ErrorCode::kConnectionReset;
        case UCS_ERR_NOT_CONNECTED:
            return zerokv::ErrorCode::kConnectionRefused;
        case UCS_ERR_MESSAGE_TRUNCATED:
            return zerokv::ErrorCode::kMessageTruncated;
        case UCS_ERR_EXCEEDS_LIMIT:
            return zerokv::ErrorCode::kOutOfMemory;
        default:
            return zerokv::ErrorCode::kTransportError;
    }
}

} // anonymous namespace

namespace zerokv {

const std::error_category& zerokv_category() noexcept {
    return get_zerokv_category();
}

// Convert UCX status to zerokv Status
Status from_ucs_status(ucs_status_t s, std::string msg = {}) {
    if (s == UCS_OK) {
        return Status::OK();
    }
    return Status(ucx_to_zerokv_error(s), std::move(msg));
}

} // namespace zerokv
