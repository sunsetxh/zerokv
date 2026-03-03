// Performance monitoring and metrics
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <string>
#include <mutex>
#include <vector>

namespace zerokv {

// Counter metric
class Counter {
public:
    void increment(int64_t delta = 1) { count_.fetch_add(delta); }
    void decrement(int64_t delta = 1) { count_.fetch_sub(delta); }
    int64_t value() const { return count_.load(); }
    void reset() { count_.store(0); }

private:
    std::atomic<int64_t> count_{0};
};

// Gauge metric
class Gauge {
public:
    void set(double value) { value_ = value; }
    void increment(double delta = 1.0) { value_ += delta; }
    void decrement(double delta = 1.0) { value_ -= delta; }
    double value() const { return value_; }

private:
    double value_ = 0.0;
};

// Histogram metric
class Histogram {
public:
    void record(double value);

    struct Stats {
        double min;
        double max;
        double avg;
        double p50;
        double p95;
        double p99;
        int64_t count;
    };

    Stats stats() const;

private:
    mutable std::mutex mutex_;
    std::vector<double> values_;
    static constexpr size_t MAX_SAMPLES = 10000;
};

// Metrics registry
class Metrics {
public:
    static Metrics& instance();

    // Get or create counter
    Counter& counter(const std::string& name);

    // Get or create gauge
    Gauge& gauge(const std::string& name);

    // Get or create histogram
    Histogram& histogram(const std::string& name);

    // Convenience methods
    void increment_counter(const std::string& name, int64_t delta = 1);
    void set_gauge(const std::string& name, double value);
    void record_histogram(const std::string& name, double value);

    // Print all metrics
    void print() const;

    // Reset all
    void reset();

private:
    Metrics() = default;

    std::unordered_map<std::string, std::unique_ptr<Counter>> counters_;
    std::unordered_map<std::string, std::unique_ptr<Gauge>> gauges_;
    std::unordered_map<std::string, std::unique_ptr<Histogram>> histograms_;
    std::mutex mutex_;
};

// Timer helper
class Timer {
public:
    Timer(const std::string& histogram_name)
        : name_(histogram_name), start_(std::chrono::high_resolution_clock::now()) {}

    ~Timer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start_).count();
        Metrics::instance().record_histogram(name_, duration);
    }

private:
    std::string name_;
    std::chrono::high_resolution_clock::time_point start_;
};

// Convenience macros
#define ZEROKV_TIMER(name) ::zerokv::Timer _timer(name)
#define ZEROKV_COUNTER(name) ::zerokv::Metrics::instance().counter(name)
#define ZEROKV_GAUGE(name) ::zerokv::Metrics::instance().gauge(name)

} // namespace zerokv
