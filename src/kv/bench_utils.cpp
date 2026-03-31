#include "kv/bench_utils.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace axon::kv::detail {
namespace {

uint64_t parse_size_token(std::string token, Status* status) {
    token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }), token.end());
    if (token.empty()) {
        *status = Status(ErrorCode::kInvalidArgument, "empty benchmark size token");
        return 0;
    }

    size_t value_end = 0;
    uint64_t value = 0;
    try {
        value = std::stoull(token, &value_end);
    } catch (const std::exception&) {
        *status = Status(ErrorCode::kInvalidArgument, "invalid benchmark size token: " + token);
        return 0;
    }
    std::string suffix = token.substr(value_end);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });

    uint64_t scale = 1;
    if (suffix.empty()) {
        scale = 1;
    } else if (suffix == "K" || suffix == "KB" || suffix == "KIB") {
        scale = 1024ull;
    } else if (suffix == "M" || suffix == "MB" || suffix == "MIB") {
        scale = 1024ull * 1024ull;
    } else if (suffix == "G" || suffix == "GB" || suffix == "GIB") {
        scale = 1024ull * 1024ull * 1024ull;
    } else {
        *status = Status(ErrorCode::kInvalidArgument, "invalid benchmark size suffix: " + suffix);
        return 0;
    }

    *status = Status::OK();
    return value * scale;
}

}  // namespace

SizeListResult parse_size_list(const std::string& text) {
    SizeListResult result;
    result.status = Status::OK();
    std::stringstream stream(text);
    std::string token;
    while (std::getline(stream, token, ',')) {
        Status status = Status::OK();
        const auto bytes = parse_size_token(token, &status);
        if (!status.ok()) {
            result.status = status;
            result.values.clear();
            return result;
        }
        result.values.push_back(bytes);
    }
    if (result.values.empty()) {
        result.status = Status(ErrorCode::kInvalidArgument, "benchmark size list is empty");
    }
    return result;
}

uint64_t derive_iterations(uint64_t size_bytes,
                           std::optional<uint64_t> explicit_iters,
                           uint64_t total_bytes) {
    if (explicit_iters.has_value()) {
        return std::max<uint64_t>(1, *explicit_iters);
    }
    return std::max<uint64_t>(1, total_bytes / std::max<uint64_t>(1, size_bytes));
}

std::string format_size(uint64_t size_bytes) {
    if (size_bytes >= 1024ull * 1024ull * 1024ull) {
        return std::to_string(size_bytes / (1024ull * 1024ull * 1024ull)) + "GiB";
    }
    if (size_bytes >= 1024ull * 1024ull) {
        return std::to_string(size_bytes / (1024ull * 1024ull)) + "MiB";
    }
    if (size_bytes >= 1024ull) {
        return std::to_string(size_bytes / 1024ull) + "KiB";
    }
    return std::to_string(size_bytes) + "B";
}

double throughput_mb_per_sec(uint64_t size_bytes, double avg_total_us) {
    if (avg_total_us <= 0.0) {
        return 0.0;
    }
    const double seconds = avg_total_us / 1'000'000.0;
    return (static_cast<double>(size_bytes) / seconds) / (1024.0 * 1024.0);
}

std::string render_publish_rows(const std::vector<PublishBenchRow>& rows) {
    std::ostringstream out;
    out << std::left
        << std::setw(10) << "size"
        << std::setw(10) << "iters"
        << std::setw(14) << "bytes"
        << std::setw(16) << "avg_total_us"
        << std::setw(18) << "avg_prepare_us"
        << std::setw(20) << "avg_pack_rkey_us"
        << std::setw(22) << "avg_put_meta_rpc_us"
        << std::setw(18) << "throughput_MiBps"
        << '\n';

    out << std::fixed << std::setprecision(2);
    for (const auto& row : rows) {
        out << std::left
            << std::setw(10) << format_size(row.size_bytes)
            << std::setw(10) << row.iterations
            << std::setw(14) << row.size_bytes
            << std::setw(16) << row.avg_total_us
            << std::setw(18) << row.avg_prepare_us
            << std::setw(20) << row.avg_pack_rkey_us
            << std::setw(22) << row.avg_put_meta_rpc_us
            << std::setw(18) << row.throughput_MiBps
            << '\n';
    }
    return out.str();
}

std::string render_fetch_rows(const std::vector<FetchBenchRow>& rows) {
    std::ostringstream out;
    out << std::left
        << std::setw(10) << "size"
        << std::setw(10) << "iters"
        << std::setw(14) << "bytes"
        << std::setw(16) << "avg_total_us"
        << std::setw(18) << "avg_prepare_us"
        << std::setw(22) << "avg_get_meta_rpc_us"
        << std::setw(20) << "avg_peer_connect_us"
        << std::setw(22) << "avg_rkey_prepare_us"
        << std::setw(20) << "avg_get_submit_us"
        << std::setw(22) << "avg_rdma_prepare_us"
        << std::setw(18) << "avg_rdma_get_us"
        << std::setw(18) << "throughput_MiBps"
        << '\n';

    out << std::fixed << std::setprecision(2);
    for (const auto& row : rows) {
        out << std::left
            << std::setw(10) << format_size(row.size_bytes)
            << std::setw(10) << row.iterations
            << std::setw(14) << row.size_bytes
            << std::setw(16) << row.avg_total_us
            << std::setw(18) << row.avg_prepare_us
            << std::setw(22) << row.avg_get_meta_rpc_us
            << std::setw(20) << row.avg_peer_connect_us
            << std::setw(22) << row.avg_rkey_prepare_us
            << std::setw(20) << row.avg_get_submit_us
            << std::setw(22) << row.avg_rdma_prepare_us
            << std::setw(18) << row.avg_rdma_get_us
            << std::setw(18) << row.throughput_MiBps
            << '\n';
    }
    return out.str();
}

}  // namespace axon::kv::detail
