#pragma once

#include <string>

namespace zerokv::internal {

inline std::string listener_host_from_address(const std::string& address) {
    const auto colon = address.rfind(':');
    if (colon == std::string::npos) {
        return address;
    }
    return address.substr(0, colon);
}

inline std::string listener_port_from_address(const std::string& address) {
    const auto colon = address.rfind(':');
    if (colon == std::string::npos || colon + 1 >= address.size()) {
        return {};
    }
    return address.substr(colon + 1);
}

inline bool is_wildcard_listener_host(const std::string& host) {
    return host.empty() || host == "0.0.0.0" || host == "::";
}

inline std::string select_listener_address(const std::string& requested,
                                           const std::string& queried) {
    const auto requested_host = listener_host_from_address(requested);
    if (is_wildcard_listener_host(requested_host)) {
        return queried;
    }

    const auto queried_port = listener_port_from_address(queried);
    if (queried_port.empty()) {
        return queried;
    }

    return requested_host + ":" + queried_port;
}

}  // namespace zerokv::internal
