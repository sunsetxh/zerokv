/**
 * Copyright (c) 2025. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ZEROKV_ZEROKV_MONITOR_H
#define ZEROKV_ZEROKV_MONITOR_H

#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>
#include <atomic>

#include "zerokv/zerokv_types.h"

namespace zerokv {

/**
 * @brief Performance Monitor
 *
 * Collects and aggregates performance metrics for ZeroKV operations.
 * Supports real-time display and Prometheus export.
 */
class PerformanceMonitor {
public:
    /**
     * @brief Constructor
     */
    PerformanceMonitor();

    /**
     * @brief Destructor
     */
    ~PerformanceMonitor();

    /**
     * @brief Record an operation
     * @param metrics Operation metrics
     */
    void RecordOperation(const OperationMetrics& metrics);

    /**
     * @brief Get aggregated stats for an operation type
     * @param type Operation type
     * @return Aggregated statistics
     */
    AggregatedStats GetStats(OperationType type) const;

    /**
     * @brief Get all aggregated stats
     * @return Map of operation type to aggregated stats
     */
    std::map<OperationType, AggregatedStats> GetAllStats() const;

    /**
     * @brief Export metrics in Prometheus format
     * @return Prometheus metrics string
     */
    std::string ExportPrometheus() const;

    /**
     * @brief Start real-time display in terminal
     *
     * Prints performance metrics to stdout every second.
     */
    void StartRealTimeDisplay();

    /**
     * @brief Stop real-time display
     */
    void StopRealTimeDisplay();

    /**
     * @brief Clear all metrics
     */
    void Clear();

    /**
     * @brief Set history window size
     * @param windowSizeSeconds Time window for metrics in seconds
     */
    void SetHistoryWindow(uint32_t windowSizeSeconds);

private:
    // Disable copy and move
    PerformanceMonitor(const PerformanceMonitor&) = delete;
    PerformanceMonitor& operator=(const PerformanceMonitor&) = delete;

    // Compute aggregated stats from metrics history
    AggregatedStats ComputeStats(const std::vector<OperationMetrics>& metrics) const;

    // Calculate percentile
    double CalculatePercentile(std::vector<uint64_t> latencies, double percentile) const;

    // Clean old metrics outside history window
    void CleanOldMetrics();

    // Real-time display thread
    void DisplayLoop();

    // Format bytes for display
    std::string FormatBytes(uint64_t bytes) const;

    // Metrics storage
    std::map<OperationType, std::vector<OperationMetrics>> metricsHistory_;
    mutable std::shared_mutex metricsMutex_;

    // Cached aggregated stats
    mutable std::map<OperationType, AggregatedStats> cachedStats_;
    mutable std::atomic<uint64_t> lastUpdateTime_{0};
    static constexpr uint64_t CACHE_VALIDITY_US = 100000;  // 100ms

    // Configuration
    uint32_t historyWindowSeconds_{60};  // Keep 60 seconds of history

    // Real-time display
    std::atomic<bool> displayRunning_{false};
    std::unique_ptr<std::thread> displayThread_;
};

}  // namespace zerokv

#endif  // ZEROKV_ZEROKV_MONITOR_H
