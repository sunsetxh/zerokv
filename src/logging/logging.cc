#include "zerokv/logging.h"
#include <fstream>

namespace zerokv {

// FileSink implementation
FileSink::FileSink(const std::string& filename) : filename_(filename) {
    stream_ = std::make_unique<std::ofstream>(filename, std::ios::app);
}

FileSink::~FileSink() {
    if (stream_ && stream_->is_open()) {
        stream_->close();
    }
}

void FileSink::write(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (stream_ && stream_->is_open()) {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::tm tm_buf;
        localtime_r(&time, &tm_buf);

        *stream_ << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
                 << "." << std::setfill('0') << std::setw(3) << ms.count()
                 << " [" << static_cast<int>(level) << "] "
                 << message << std::endl;
        stream_->flush();
    }
}

// Logger implementation
Logger& Logger::instance() {
    static Logger instance;
    return instance;
}

Logger::Logger() {
    // Add default console sink
    add_sink(std::make_unique<ConsoleSink>());
}

void Logger::add_sink(std::unique_ptr<LogSink> sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.push_back(std::move(sink));
}

void Logger::set_level(LogLevel level) {
    min_level_ = level;
}

void Logger::write(LogLevel level, const std::string& message) {
    if (level < min_level_) return;

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& sink : sinks_) {
        sink->write(level, message);
    }
}

void Logger::debug(const std::string& msg) { write(LogLevel::DEBUG, msg); }
void Logger::info(const std::string& msg) { write(LogLevel::INFO, msg); }
void Logger::warning(const std::string& msg) { write(LogLevel::WARNING, msg); }
void Logger::error(const std::string& msg) { write(LogLevel::ERROR, msg); }
void Logger::fatal(const std::string& msg) { write(LogLevel::FATAL, msg); }

} // namespace zerokv
