#pragma once

#include <iostream>
#include <mutex>
#include <string>

namespace zerokv::detail {

inline std::mutex& trace_log_mutex() {
    static std::mutex mu;
    return mu;
}

inline void write_trace_line(const std::string& line) {
    std::lock_guard<std::mutex> lock(trace_log_mutex());
    std::cerr << line << '\n';
}

}  // namespace zerokv::detail
