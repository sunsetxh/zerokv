#pragma once

#include <cstdint>
#include <mutex>
#include <string_view>

namespace zerokv::detail {

enum class LogLevel : uint8_t {
    kError = 0,
    kWarn = 1,
    kInfo = 2,
    kDebug = 3,
    kTrace = 4,
};

const char* log_level_name(LogLevel level) noexcept;
LogLevel parse_log_level(std::string_view value) noexcept;
LogLevel current_log_level();
bool log_enabled(LogLevel level);
bool log_component_enabled(std::string_view component);
void reset_log_level_for_tests();

std::mutex& log_output_mutex();
void write_raw_log_line(std::string_view line);
void write_log_line(LogLevel level, std::string_view component, std::string_view message);

}  // namespace zerokv::detail
