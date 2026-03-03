// Utility functions
#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <random>
#include <sstream>
#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace zerokv {

// String utilities
namespace str {

// Trim whitespace
inline std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

// Split string
inline std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> result;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delimiter)) {
        result.push_back(item);
    }
    return result;
}

// Join string
inline std::string join(const std::vector<std::string>& items, const std::string& separator) {
    if (items.empty()) return "";
    std::stringstream ss;
    ss << items[0];
    for (size_t i = 1; i < items.size(); i++) {
        ss << separator << items[i];
    }
    return ss.str();
}

// To lowercase
inline std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

// To uppercase
inline std::string to_upper(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(), ::toupper);
    return result;
}

// Format string
inline std::string format(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return buffer;
}

} // namespace str

// Time utilities
namespace time {

// Get current timestamp in milliseconds
inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Get current timestamp in microseconds
inline int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Get current timestamp in nanoseconds
inline int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Format duration
inline std::string format_duration(int64_t ms) {
    if (ms < 1000) return std::to_string(ms) + "ms";
    if (ms < 60000) return std::to_string(ms / 1000.0) + "s";
    if (ms < 3600000) return std::to_string(ms / 60000.0) + "min";
    return std::to_string(ms / 3600000.0) + "h";
}

} // namespace time

// Random utilities
namespace random {

// Generate random string
inline std::string generate_string(size_t length) {
    static const char charset[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);

    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; i++) {
        result += charset[dist(gen)];
    }
    return result;
}

// Generate random number
inline int generate_int(int min, int max) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(min, max);
    return dist(gen);
}

} // namespace random

// Byte utilities
namespace bytes {

// Format bytes to human readable string
inline std::string format(size_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double size = bytes;

    while (size >= 1024 && unit < 4) {
        size /= 1024;
        unit++;
    }

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.2f %s", size, units[unit]);
    return buffer;
}

// Parse human readable string to bytes
inline size_t parse(const std::string& s) {
    size_t multiplier = 1;
    std::string num = s;

    if (s.find("TB") != std::string::npos) {
        multiplier = 1024ULL * 1024 * 1024 * 1024;
        num = s.substr(0, s.find("TB"));
    } else if (s.find("GB") != std::string::npos) {
        multiplier = 1024ULL * 1024 * 1024;
        num = s.substr(0, s.find("GB"));
    } else if (s.find("MB") != std::string::npos) {
        multiplier = 1024ULL * 1024;
        num = s.substr(0, s.find("MB"));
    } else if (s.find("KB") != std::string::npos) {
        multiplier = 1024;
        num = s.substr(0, s.find("KB"));
    }

    return static_cast<size_t>(std::stod(num) * multiplier);
}

} // namespace bytes

} // namespace zerokv
