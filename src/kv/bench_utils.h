#pragma once

#include "axon/common.h"

#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace axon::kv::detail {

struct SizeListResult {
    Status status;
    std::vector<uint64_t> values;

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
    [[nodiscard]] const std::vector<uint64_t>& value() const noexcept { return values; }
};

SizeListResult parse_size_list(const std::string& text);
uint64_t derive_iterations(uint64_t size_bytes,
                           std::optional<uint64_t> explicit_iters,
                           uint64_t total_bytes);
std::string format_size(uint64_t size_bytes);

struct PublishBenchRow {
    uint64_t size_bytes = 0;
    uint64_t iterations = 0;
    double avg_total_us = 0.0;
    double avg_prepare_us = 0.0;
    double avg_pack_rkey_us = 0.0;
    double avg_put_meta_rpc_us = 0.0;
    double throughput_MiBps = 0.0;
};

struct FetchBenchRow {
    uint64_t size_bytes = 0;
    uint64_t iterations = 0;
    double avg_total_us = 0.0;
    double avg_prepare_us = 0.0;
    double avg_get_meta_rpc_us = 0.0;
    double avg_peer_connect_us = 0.0;
    double avg_rkey_prepare_us = 0.0;
    double avg_get_submit_us = 0.0;
    double avg_rdma_prepare_us = 0.0;
    double avg_rdma_get_us = 0.0;
    double throughput_MiBps = 0.0;
};

double throughput_mb_per_sec(uint64_t size_bytes, double avg_total_us);
std::string render_publish_rows(const std::vector<PublishBenchRow>& rows);
std::string render_fetch_rows(const std::vector<FetchBenchRow>& rows);

}  // namespace axon::kv::detail
