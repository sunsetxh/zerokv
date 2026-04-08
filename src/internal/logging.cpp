#include "internal/logging.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace zerokv::detail {

namespace {

struct LogLevelState {
    std::mutex mu;
    bool initialized = false;
    LogLevel level = LogLevel::kError;
    std::vector<std::string> components;
};

LogLevelState& log_level_state() {
    static LogLevelState state;
    return state;
}

std::string ascii_lower(std::string_view value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lowered;
}

std::string trim_ascii(std::string_view value) {
    size_t start = 0;
    size_t end = value.size();
    while (start < end && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

std::vector<std::string> parse_component_filters(std::string_view value) {
    std::vector<std::string> filters;
    std::string current;
    std::istringstream input{std::string(value)};
    while (std::getline(input, current, ',')) {
        auto trimmed = trim_ascii(current);
        if (!trimmed.empty()) {
            filters.push_back(std::move(trimmed));
        }
    }
    return filters;
}

}  // namespace

const char* log_level_name(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::kError:
            return "error";
        case LogLevel::kWarn:
            return "warn";
        case LogLevel::kInfo:
            return "info";
        case LogLevel::kDebug:
            return "debug";
        case LogLevel::kTrace:
            return "trace";
    }
    return "error";
}

LogLevel parse_log_level(std::string_view value) noexcept {
    const std::string lowered = ascii_lower(value);
    if (lowered == "error") {
        return LogLevel::kError;
    }
    if (lowered == "warn" || lowered == "warning") {
        return LogLevel::kWarn;
    }
    if (lowered == "info") {
        return LogLevel::kInfo;
    }
    if (lowered == "debug") {
        return LogLevel::kDebug;
    }
    if (lowered == "trace") {
        return LogLevel::kTrace;
    }
    return LogLevel::kError;
}

LogLevel current_log_level() {
    auto& state = log_level_state();
    std::lock_guard<std::mutex> lock(state.mu);
    if (!state.initialized) {
        const char* level = std::getenv("ZEROKV_LOG_LEVEL");
        const char* components = std::getenv("ZEROKV_LOG_COMPONENTS");
        state.level = level != nullptr ? parse_log_level(level) : LogLevel::kError;
        state.components =
            components != nullptr ? parse_component_filters(components) : std::vector<std::string>{};
        state.initialized = true;
    }
    return state.level;
}

bool log_enabled(LogLevel level) {
    return static_cast<uint8_t>(level) <= static_cast<uint8_t>(current_log_level());
}

bool log_component_enabled(std::string_view component) {
    (void)current_log_level();
    auto& state = log_level_state();
    std::lock_guard<std::mutex> lock(state.mu);
    if (state.components.empty()) {
        return true;
    }
    for (const auto& filter : state.components) {
        if (component == filter) {
            return true;
        }
        if (component.size() > filter.size() &&
            component.substr(0, filter.size()) == filter) {
            return true;
        }
    }
    return false;
}

void reset_log_level_for_tests() {
    auto& state = log_level_state();
    std::lock_guard<std::mutex> lock(state.mu);
    state.initialized = false;
    state.level = LogLevel::kError;
    state.components.clear();
}

std::mutex& log_output_mutex() {
    static std::mutex mu;
    return mu;
}

void write_raw_log_line(std::string_view line) {
    std::lock_guard<std::mutex> lock(log_output_mutex());
    std::cerr << line << '\n';
}

void write_log_line(LogLevel level, std::string_view component, std::string_view message) {
    if (!log_enabled(level) || !log_component_enabled(component)) {
        return;
    }

    std::string line;
    line.reserve(component.size() + message.size() + 32);
    line += "[zerokv][";
    line += log_level_name(level);
    line += "][";
    line += component;
    line += "] ";
    line += message;
    write_raw_log_line(line);
}

}  // namespace zerokv::detail
