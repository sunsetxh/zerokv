#ifndef ZEROKV_LOGGING_H
#define ZEROKV_LOGGING_H

#include <iostream>
#include <sstream>
#include <memory>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <vector>
#include <fstream>

namespace zerokv {

// Log levels
enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARNING = 2,
    ERROR = 3,
    FATAL = 4
};

// Log output interface
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(LogLevel level, const std::string& message) = 0;
};

// Console sink
class ConsoleSink : public LogSink {
public:
    void write(LogLevel level, const std::string& message) override {
        static const char* level_str[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
        std::cout << "[" << level_str[static_cast<int>(level)] << "] "
                  << message << std::endl;
    }
};

// File sink
class FileSink : public LogSink {
public:
    FileSink(const std::string& filename);
    ~FileSink();

    void write(LogLevel level, const std::string& message) override;

private:
    std::string filename_;
    std::mutex mutex_;
    std::unique_ptr<std::ofstream> stream_;
};

// Logger
class Logger {
public:
    static Logger& instance();

    void add_sink(std::unique_ptr<LogSink> sink);
    void set_level(LogLevel level);

    void debug(const std::string& msg);
    void info(const std::string& msg);
    void warning(const std::string& msg);
    void error(const std::string& msg);
    void fatal(const std::string& msg);

    // Template methods for formatted logging
    template<typename... Args>
    void log(LogLevel level, Args&&... args) {
        if (level < min_level_) return;

        std::ostringstream oss;
        ((oss << std::forward<Args>(args)), ...);
        write(level, oss.str());
    }

private:
    Logger();
    ~Logger() = default;

    void write(LogLevel level, const std::string& message);

    std::vector<std::unique_ptr<LogSink>> sinks_;
    LogLevel min_level_ = LogLevel::INFO;
    std::mutex mutex_;
};

// Convenience macros
#define ZEROKV_DEBUG(...) ::zerokv::Logger::instance().log(::zerokv::LogLevel::DEBUG, __VA_ARGS__)
#define ZEROKV_INFO(...) ::zerokv::Logger::instance().log(::zerokv::LogLevel::INFO, __VA_ARGS__)
#define ZEROKV_WARN(...) ::zerokv::Logger::instance().log(::zerokv::LogLevel::WARNING, __VA_ARGS__)
#define ZEROKV_ERROR(...) ::zerokv::Logger::instance().log(::zerokv::LogLevel::ERROR, __VA_ARGS__)
#define ZEROKV_FATAL(...) ::zerokv::Logger::instance().log(::zerokv::LogLevel::FATAL, __VA_ARGS__)

} // namespace zerokv

#endif // ZEROKV_LOGGING_H
