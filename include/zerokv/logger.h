// Copyright 2025 ZeroKV Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#ifndef ZEROKV_LOGGER_H
#define ZEROKV_LOGGER_H

#include <string>
#include <memory>
#include <mutex>
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <thread>

namespace zerokv {

// 日志级别
enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3,
    NONE = 4  // 禁用所有日志
};

// 日志输出目标
enum class LogTarget {
    STDOUT,   // 标准输出
    STDERR,   // 标准错误
    FILE,     // 文件
    CUSTOM    // 自定义输出
};

// 日志记录器接口
class Logger {
public:
    virtual ~Logger() = default;

    // 记录日志
    virtual void Log(LogLevel level, const char* file, int line,
                     const std::string& message) = 0;

    // 设置日志级别
    virtual void SetLevel(LogLevel level) = 0;

    // 获取当前日志级别
    virtual LogLevel GetLevel() const = 0;

    // 刷新缓冲
    virtual void Flush() = 0;
};

// 默认日志记录器实现
class DefaultLogger : public Logger {
public:
    DefaultLogger(LogTarget target = LogTarget::STDOUT,
                  LogLevel level = LogLevel::INFO);

    // 构造函数：输出到文件
    explicit DefaultLogger(const std::string& filename,
                          LogLevel level = LogLevel::INFO);

    ~DefaultLogger() override;

    void Log(LogLevel level, const char* file, int line,
             const std::string& message) override;

    void SetLevel(LogLevel level) override;
    LogLevel GetLevel() const override;
    void Flush() override;

    // 设置是否显示时间戳
    void SetShowTimestamp(bool show) { show_timestamp_ = show; }

    // 设置是否显示线程 ID
    void SetShowThreadId(bool show) { show_thread_id_ = show; }

    // 设置是否显示文件名和行号
    void SetShowLocation(bool show) { show_location_ = show; }

private:
    std::string FormatMessage(LogLevel level, const char* file, int line,
                             const std::string& message);
    std::string GetLevelString(LogLevel level) const;
    std::string GetTimestamp() const;
    std::string GetThreadId() const;
    std::string GetFileName(const char* file) const;

    LogTarget target_;
    LogLevel level_;
    std::mutex mutex_;
    std::unique_ptr<std::ofstream> file_stream_;

    bool show_timestamp_ = true;
    bool show_thread_id_ = false;
    bool show_location_ = true;
};

// 全局日志管理器
class LogManager {
public:
    static LogManager& Instance();

    // 设置日志记录器
    void SetLogger(std::shared_ptr<Logger> logger);

    // 获取日志记录器
    std::shared_ptr<Logger> GetLogger();

    // 便捷方法：设置日志级别
    void SetLevel(LogLevel level);

    // 便捷方法：设置输出到文件
    void SetOutputFile(const std::string& filename);

    // 便捷方法：设置输出到 stdout/stderr
    void SetOutputTarget(LogTarget target);

private:
    LogManager();
    ~LogManager() = default;

    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    std::shared_ptr<Logger> logger_;
    std::mutex mutex_;
};

// 日志宏定义
#define LOG_DEBUG(msg) \
    do { \
        auto logger = zerokv::LogManager::Instance().GetLogger(); \
        if (logger && logger->GetLevel() <= zerokv::LogLevel::DEBUG) { \
            std::ostringstream oss; \
            oss << msg; \
            logger->Log(zerokv::LogLevel::DEBUG, __FILE__, __LINE__, oss.str()); \
        } \
    } while(0)

#define LOG_INFO(msg) \
    do { \
        auto logger = zerokv::LogManager::Instance().GetLogger(); \
        if (logger && logger->GetLevel() <= zerokv::LogLevel::INFO) { \
            std::ostringstream oss; \
            oss << msg; \
            logger->Log(zerokv::LogLevel::INFO, __FILE__, __LINE__, oss.str()); \
        } \
    } while(0)

#define LOG_WARN(msg) \
    do { \
        auto logger = zerokv::LogManager::Instance().GetLogger(); \
        if (logger && logger->GetLevel() <= zerokv::LogLevel::WARN) { \
            std::ostringstream oss; \
            oss << msg; \
            logger->Log(zerokv::LogLevel::WARN, __FILE__, __LINE__, oss.str()); \
        } \
    } while(0)

#define LOG_ERROR(msg) \
    do { \
        auto logger = zerokv::LogManager::Instance().GetLogger(); \
        if (logger && logger->GetLevel() <= zerokv::LogLevel::ERROR) { \
            std::ostringstream oss; \
            oss << msg; \
            logger->Log(zerokv::LogLevel::ERROR, __FILE__, __LINE__, oss.str()); \
        } \
    } while(0)

// 带格式化的日志宏
#define LOG_DEBUG_FMT(fmt, ...) \
    do { \
        auto logger = zerokv::LogManager::Instance().GetLogger(); \
        if (logger && logger->GetLevel() <= zerokv::LogLevel::DEBUG) { \
            char buf[1024]; \
            snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__); \
            logger->Log(zerokv::LogLevel::DEBUG, __FILE__, __LINE__, buf); \
        } \
    } while(0)

#define LOG_INFO_FMT(fmt, ...) \
    do { \
        auto logger = zerokv::LogManager::Instance().GetLogger(); \
        if (logger && logger->GetLevel() <= zerokv::LogLevel::INFO) { \
            char buf[1024]; \
            snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__); \
            logger->Log(zerokv::LogLevel::INFO, __FILE__, __LINE__, buf); \
        } \
    } while(0)

#define LOG_WARN_FMT(fmt, ...) \
    do { \
        auto logger = zerokv::LogManager::Instance().GetLogger(); \
        if (logger && logger->GetLevel() <= zerokv::LogLevel::WARN) { \
            char buf[1024]; \
            snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__); \
            logger->Log(zerokv::LogLevel::WARN, __FILE__, __LINE__, buf); \
        } \
    } while(0)

#define LOG_ERROR_FMT(fmt, ...) \
    do { \
        auto logger = zerokv::LogManager::Instance().GetLogger(); \
        if (logger && logger->GetLevel() <= zerokv::LogLevel::ERROR) { \
            char buf[1024]; \
            snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__); \
            logger->Log(zerokv::LogLevel::ERROR, __FILE__, __LINE__, buf); \
        } \
    } while(0)

}  // namespace zerokv

#endif  // ZEROKV_LOGGER_H
