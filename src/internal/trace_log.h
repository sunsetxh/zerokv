#pragma once

#include "internal/logging.h"

#include <string>

namespace zerokv::detail {

inline void write_trace_line(const std::string& line) {
    write_raw_log_line(line);
}

}  // namespace zerokv::detail
