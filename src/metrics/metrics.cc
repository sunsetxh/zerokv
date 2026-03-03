#include "zerokv/metrics.h"
#include <iostream>
#include <algorithm>
#include <numeric>

namespace zerokv {

// Histogram implementation
void Histogram::record(double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (values_.size() >= MAX_SAMPLES) {
        values_.erase(values_.begin());
    }
    values_.push_back(value);
}

Histogram::Stats Histogram::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    Stats s;
    if (values_.empty()) {
        s.min = s.max = s.avg = s.p50 = s.p95 = s.p99 = 0;
        s.count = 0;
        return s;
    }

    std::vector<double> sorted = values_;
    std::sort(sorted.begin(), sorted.end());

    s.min = sorted.front();
    s.max = sorted.back();
    s.avg = std::accumulate(sorted.begin(), sorted.end(), 0.0) / sorted.size();
    s.count = sorted.size();

    size_t idx50 = sorted.size() * 50 / 100;
    size_t idx95 = sorted.size() * 95 / 100;
    size_t idx99 = sorted.size() * 99 / 100;

    s.p50 = sorted[idx50];
    s.p95 = sorted[idx95];
    s.p99 = sorted[idx99];

    return s;
}

// Metrics implementation
Metrics& Metrics::instance() {
    static Metrics instance;
    return instance;
}

Counter& Metrics::counter(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = counters_.find(name);
    if (it != counters_.end()) {
        return *it->second;
    }
    auto counter = std::make_unique<Counter>();
    auto* ptr = counter.get();
    counters_[name] = std::move(counter);
    return *ptr;
}

Gauge& Metrics::gauge(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = gauges_.find(name);
    if (it != gauges_.end()) {
        return *it->second;
    }
    auto gauge = std::make_unique<Gauge>();
    auto* ptr = gauge.get();
    gauges_[name] = std::move(gauge);
    return *ptr;
}

Histogram& Metrics::histogram(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = histograms_.find(name);
    if (it != histograms_.end()) {
        return *it->second;
    }
    auto histogram = std::make_unique<Histogram>();
    auto* ptr = histogram.get();
    histograms_[name] = std::move(histogram);
    return *ptr;
}

void Metrics::increment_counter(const std::string& name, int64_t delta) {
    counter(name).increment(delta);
}

void Metrics::set_gauge(const std::string& name, double value) {
    gauge(name).set(value);
}

void Metrics::record_histogram(const std::string& name, double value) {
    histogram(name).record(value);
}

void Metrics::print() const {
    std::cout << "\n=== Metrics ===" << std::endl;

    std::cout << "\nCounters:" << std::endl;
    for (const auto& [name, counter] : counters_) {
        std::cout << "  " << name << ": " << counter->value() << std::endl;
    }

    std::cout << "\nGauges:" << std::endl;
    for (const auto& [name, gauge] : gauges_) {
        std::cout << "  " << name << ": " << gauge->value() << std::endl;
    }

    std::cout << "\nHistograms:" << std::endl;
    for (const auto& [name, histogram] : histograms_) {
        auto s = histogram->stats();
        std::cout << "  " << name << ":" << std::endl;
        std::cout << "    count: " << s.count << std::endl;
        std::cout << "    avg: " << s.avg << " ms" << std::endl;
        std::cout << "    p50: " << s.p50 << " ms" << std::endl;
        std::cout << "    p95: " << s.p95 << " ms" << std::endl;
        std::cout << "    p99: " << s.p99 << " ms" << std::endl;
    }
}

void Metrics::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [_, counter] : counters_) {
        counter->reset();
    }
    for (auto& [_, gauge] : gauges_) {
        gauge->set(0);
    }
    histograms_.clear();
}

} // namespace zerokv
