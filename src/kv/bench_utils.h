#pragma once

#include "axon/common.h"

#include <cstdint>
#include <optional>
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

}  // namespace axon::kv::detail
