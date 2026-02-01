// Copyright 2025 ZeroKV Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#include <zerokv/logger.h>
#include <iostream>
#include <iomanip>
#include <string>

namespace zerokv {

// ============================================================================
// DefaultLogger Implementation
// ============================================================================

DefaultLogger::DefaultLogger(LogTarget target, LogLevel level)
    : target_(target), level_(level) {
}

DefaultLogger::DefaultLogger(const std::string& filename, LogLevel level)
    : target_(LogTarget::FILE), level_(level) {
    file_stream_ = std::make_unique<std::ofstream>(filename, std::ios::app);
    if (!file_stream_->is_open()) {
        // 回退到 stderr
        target_ = LogTarget::STDERR;
        std::cerr << "Failed to open log file: " << filename
                  << ", falling back to stderr" << std::endl;
    }
}

DefaultLogger::~DefaultLogger() {
    Flush();
    if (file_stream_ && file_stream_->is_open()) {
        file_stream_->close();
    }
}

void DefaultLogger::Log(LogLevel level, const char* file, int line,
                       const std::string& message) {
    if (level < level_) {
        return;
    }

    std::string formatted = FormatMessage(level, file, line, message);

    std::lock_guard<std::mutex> lock(mutex_);

    switch (target_) {
        case LogTarget::STDOUT:
            std::cout << formatted << std::endl;
            break;
        case LogTarget::STDERR:
            std::cerr << formatted << std::endl;
            break;
        case LogTarget::FILE:
            if (file_stream_ && file_stream_->is_open()) {
                *file_stream_ << formatted << std::endl;
            } else {
                std::cerr << formatted << std::endl;
            }
            break;
        default:
            break;
    }
}

void DefaultLogger::SetLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

LogLevel DefaultLogger::GetLevel() const {
    return level_;
}

void DefaultLogger::Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_stream_) {
        file_stream_->flush();
    }
    std::cout.flush();
    std::cerr.flush();
}

std::string DefaultLogger::FormatMessage(LogLevel level, const char* file,
                                        int line, const std::string& message) {
    std::ostringstream oss;

    // 时间戳
    if (show_timestamp_) {
        oss << "[" << GetTimestamp() << "] ";
    }

    // 日志级别
    oss << "[" << GetLevelString(level) << "] ";

    // 线程 ID
    if (show_thread_id_) {
        oss << "[" << GetThreadId() << "] ";
    }

    // 文件位置
    if (show_location_ && file) {
        oss << "[" << GetFileName(file) << ":" << line << "] ";
    }

    // 消息
    oss << message;

    return oss.str();
}

std::string DefaultLogger::GetLevelString(LogLevel level) const {
    switch (level) {
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::INFO:
            return "INFO ";
        case LogLevel::WARN:
            return "WARN ";
        case LogLevel::ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

std::string DefaultLogger::GetTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    struct tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_now);
#endif

    oss << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count();

    return oss.str();
}

std::string DefaultLogger::GetThreadId() const {
    std::ostringstream oss;
    oss << std::this_thread::get_id();
    return oss.str();
}

std::string DefaultLogger::GetFileName(const char* file) const {
    if (!file) {
        return "";
    }

    std::string path(file);
    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos) {
        return path.substr(pos + 1);
    }
    return path;
}

// ============================================================================
// LogManager Implementation
// ============================================================================

LogManager& LogManager::Instance() {
    static LogManager instance;
    return instance;
}

LogManager::LogManager() {
    // 默认使用 stdout，INFO 级别
    logger_ = std::make_shared<DefaultLogger>(LogTarget::STDOUT, LogLevel::INFO);
}

void LogManager::SetLogger(std::shared_ptr<Logger> logger) {
    std::lock_guard<std::mutex> lock(mutex_);
    logger_ = logger;
}

std::shared_ptr<Logger> LogManager::GetLogger() {
    std::lock_guard<std::mutex> lock(mutex_);
    return logger_;
}

void LogManager::SetLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logger_) {
        logger_->SetLevel(level);
    }
}

void LogManager::SetOutputFile(const std::string& filename) {
    std::lock_guard<std::mutex> lock(mutex_);
    logger_ = std::make_shared<DefaultLogger>(filename);
}

void LogManager::SetOutputTarget(LogTarget target) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logger_) {
        LogLevel current_level = logger_->GetLevel();
        logger_ = std::make_shared<DefaultLogger>(target, current_level);
    } else {
        logger_ = std::make_shared<DefaultLogger>(target);
    }
}

}  // namespace zerokv
