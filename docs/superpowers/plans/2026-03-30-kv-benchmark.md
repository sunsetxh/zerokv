# KV Benchmark Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a dedicated `kv_bench` executable for two-node publish/fetch size-sweep benchmarks, using the existing KV server/node stack and current publish/fetch metrics.

**Architecture:** Add a standalone `kv_bench` CLI with four modes: `server`, `hold-owner`, `bench-publish`, and `bench-fetch`. Put parsing, size normalization, iteration calculation, and table formatting in a small reusable benchmark helper module so the executable and tests share the same logic. Keep the benchmark independent from `kv_demo`, but reuse `KVServer`, `KVNode`, `PublishMetrics`, and `FetchMetrics`.

**Tech Stack:** C++17, CMake, ZeroKV KV API (`KVServer`, `KVNode`), GoogleTest, UCX-backed ZeroKV transport

---

## File Structure

- Create: `examples/kv_bench.cpp`
  - Main benchmark CLI with the four modes and top-level orchestration.
- Create: `src/kv/bench_utils.h`
  - Internal declarations for size parsing, iteration calculation, benchmark rows, and table rendering.
- Create: `src/kv/bench_utils.cpp`
  - Internal implementation shared by the executable and tests.
- Create: `tests/unit/test_kv_bench.cpp`
  - Unit coverage for size parsing and iteration planning.
- Create: `tests/integration/test_kv_bench.cpp`
  - Local smoke coverage for one-size publish/fetch benchmark flows over TCP.
- Modify: `CMakeLists.txt`
  - Add `kv_bench` example target and new unit/integration test targets.

### Task 1: Add Shared Benchmark Helpers

**Files:**
- Create: `src/kv/bench_utils.h`
- Create: `src/kv/bench_utils.cpp`
- Test: `tests/unit/test_kv_bench.cpp`

- [ ] **Step 1: Write the failing unit tests**

```cpp
#include "kv/bench_utils.h"

#include <gtest/gtest.h>

namespace zerokv::kv::detail {

TEST(KvBenchUtilsTest, ParseSizeListSupportsBinarySuffixes) {
    const auto sizes = parse_size_list("4K,64K,1M,32M");
    ASSERT_TRUE(sizes.ok()) << sizes.status().message();
    EXPECT_EQ(sizes.value().size(), 4u);
    EXPECT_EQ(sizes.value()[0], 4ull * 1024ull);
    EXPECT_EQ(sizes.value()[1], 64ull * 1024ull);
    EXPECT_EQ(sizes.value()[2], 1ull * 1024ull * 1024ull);
    EXPECT_EQ(sizes.value()[3], 32ull * 1024ull * 1024ull);
}

TEST(KvBenchUtilsTest, ParseSizeListRejectsInvalidToken) {
    const auto sizes = parse_size_list("4K,boom,1M");
    EXPECT_FALSE(sizes.ok());
}

TEST(KvBenchUtilsTest, DeriveIterationsUsesTotalBytesAndMinOne) {
    EXPECT_EQ(derive_iterations(4ull * 1024ull, std::nullopt, 1ull << 30), 262144u);
    EXPECT_EQ(derive_iterations(128ull * 1024ull * 1024ull, std::nullopt, 1ull << 30), 8u);
    EXPECT_EQ(derive_iterations(2ull * 1024ull * 1024ull * 1024ull, std::nullopt, 1ull << 30), 1u);
}

TEST(KvBenchUtilsTest, DeriveIterationsPrefersExplicitIters) {
    EXPECT_EQ(derive_iterations(4ull * 1024ull, 7u, 1ull << 30), 7u);
}

}  // namespace zerokv::kv::detail
```

- [ ] **Step 2: Run the unit test to confirm it fails**

Run:

```bash
cmake --build build --target test_kv_bench -j4
```

Expected:

- build fails because `src/kv/bench_utils.h`, `src/kv/bench_utils.cpp`, and `test_kv_bench` do not exist yet

- [ ] **Step 3: Write the helper declarations**

```cpp
#pragma once

#include "zerokv/status.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace zerokv::kv::detail {

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

}  // namespace zerokv::kv::detail
```

- [ ] **Step 4: Write the minimal helper implementation**

```cpp
#include "kv/bench_utils.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace zerokv::kv::detail {
namespace {

uint64_t parse_size_token(std::string token, Status* status) {
    token.erase(std::remove_if(token.begin(), token.end(), ::isspace), token.end());
    if (token.empty()) {
        *status = Status(ErrorCode::kInvalidArgument, "empty benchmark size token");
        return 0;
    }

    size_t value_end = 0;
    const auto value = std::stoull(token, &value_end);
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
    *status = Status::ok();
    return value * scale;
}

}  // namespace

SizeListResult parse_size_list(const std::string& text) {
    SizeListResult result{.status = Status::ok()};
    std::stringstream stream(text);
    std::string token;
    while (std::getline(stream, token, ',')) {
        Status status = Status::ok();
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

}  // namespace zerokv::kv::detail
```

- [ ] **Step 5: Add the new unit test target**

```cmake
add_executable(test_kv_bench
    tests/unit/test_kv_bench.cpp
    src/kv/bench_utils.cpp
)
target_link_libraries(test_kv_bench PRIVATE zerokv GTest::gtest GTest::gtest_main)
target_include_directories(test_kv_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME UnitKvBench COMMAND test_kv_bench)
```

- [ ] **Step 6: Run the helper tests and verify they pass**

Run:

```bash
cmake --build build --target test_kv_bench -j4
cd build && ctest -R UnitKvBench --output-on-failure
```

Expected:

- `UnitKvBench` passes

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt src/kv/bench_utils.h src/kv/bench_utils.cpp tests/unit/test_kv_bench.cpp
git commit -m "Add KV benchmark helper utilities"
```

### Task 2: Add `kv_bench` Server and Hold-Owner Modes

**Files:**
- Create: `examples/kv_bench.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/integration/test_kv_bench.cpp`

- [ ] **Step 1: Write the failing integration smoke test for server and hold-owner**

```cpp
#include "zerokv/kv.h"

#include <gtest/gtest.h>

TEST(KvBenchIntegrationTest, HoldOwnerPublishesStableKeys) {
    const auto cfg = zerokv::Config::builder().set_transport("tcp").build();

    auto server = zerokv::kv::KVServer::create(cfg);
    ASSERT_TRUE(server->start(zerokv::kv::ServerConfig{"127.0.0.1:0"}).ok());

    auto owner = zerokv::kv::KVNode::create(cfg);
    ASSERT_TRUE(owner->start(zerokv::kv::NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "bench-owner",
    }).ok());

    auto publish = owner->publish("bench-fetch-4096", "xxxx", 4);
    ASSERT_TRUE(publish.status().ok());
    publish.get();

    auto info = server->lookup("bench-fetch-4096");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->key, "bench-fetch-4096");

    owner->stop();
    server->stop();
}
```

- [ ] **Step 2: Run the integration test to confirm the benchmark target is still missing**

Run:

```bash
cmake --build build --target test_kv_bench -j4
```

Expected:

- build succeeds for tests, but there is still no `kv_bench` target and no benchmark executable implementation

- [ ] **Step 3: Create the `kv_bench` skeleton with common argument parsing**

```cpp
#include "zerokv/kv.h"
#include "kv/bench_utils.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

using namespace zerokv;
using namespace zerokv::kv;

namespace {

std::atomic<bool> g_stop{false};

void signal_handler(int) { g_stop.store(true); }

void wait_until_stopped() {
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode;
    std::string listen_addr;
    std::string server_addr;
    std::string data_addr;
    std::string node_id;
    std::string sizes_arg;
    std::string transport = "tcp";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) mode = argv[++i];
        else if (arg == "--listen" && i + 1 < argc) listen_addr = argv[++i];
        else if (arg == "--server-addr" && i + 1 < argc) server_addr = argv[++i];
        else if (arg == "--data-addr" && i + 1 < argc) data_addr = argv[++i];
        else if (arg == "--node-id" && i + 1 < argc) node_id = argv[++i];
        else if (arg == "--sizes" && i + 1 < argc) sizes_arg = argv[++i];
        else if (arg == "--transport" && i + 1 < argc) transport = argv[++i];
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    if (mode == "server") {
        auto server = KVServer::create(Config::builder().set_transport(transport).build());
        const auto status = server->start(ServerConfig{listen_addr});
        if (!status.ok()) {
            std::cerr << status.message() << "\n";
            return 1;
        }
        std::cout << "kv_bench server listening on " << server->address() << "\n";
        wait_until_stopped();
        server->stop();
        return 0;
    }

    if (mode == "hold-owner") {
        auto node = KVNode::create(Config::builder().set_transport(transport).build());
        const auto status = node->start(NodeConfig{
            .server_addr = server_addr,
            .local_data_addr = data_addr,
            .node_id = node_id,
        });
        if (!status.ok()) {
            std::cerr << status.message() << "\n";
            return 1;
        }
        std::cout << "hold-owner ready: " << node->node_id() << "\n";
        wait_until_stopped();
        node->stop();
        return 0;
    }

    std::cerr << "unimplemented mode\n";
    return 1;
}
```

- [ ] **Step 4: Add hold-owner dataset preparation**

```cpp
const auto sizes = sizes_arg.empty()
    ? detail::parse_size_list("4K,64K,1M,4M,16M,32M,64M,128M")
    : detail::parse_size_list(sizes_arg);
if (!sizes.ok()) {
    std::cerr << sizes.status.message() << "\n";
    return 1;
}

for (const auto size_bytes : sizes.value()) {
    std::vector<std::byte> payload(size_bytes);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i % 251);
    }
    const auto key = "bench-fetch-" + std::to_string(size_bytes);
    auto publish = node->publish(key, payload.data(), payload.size());
    if (!publish.status().ok()) {
        std::cerr << "hold-owner publish failed for " << key << ": "
                  << publish.status().message() << "\n";
        node->stop();
        return 1;
    }
    publish.get();
}
```

- [ ] **Step 5: Add the example target**

```cmake
add_executable(kv_bench
    examples/kv_bench.cpp
    src/kv/bench_utils.cpp
)
target_link_libraries(kv_bench PRIVATE zerokv)
target_include_directories(kv_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)
```

- [ ] **Step 6: Run the smoke path**

Run:

```bash
cmake --build build --target kv_bench test_kv_bench -j4
cd build && ctest -R "UnitKvBench|IntegrationKvBench" --output-on-failure
```

Expected:

- `kv_bench` builds
- helper and smoke tests pass

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt examples/kv_bench.cpp tests/integration/test_kv_bench.cpp
git commit -m "Add KV benchmark server and owner modes"
```

### Task 3: Implement `bench-publish`

**Files:**
- Modify: `examples/kv_bench.cpp`
- Modify: `src/kv/bench_utils.h`
- Modify: `src/kv/bench_utils.cpp`
- Test: `tests/integration/test_kv_bench.cpp`

- [ ] **Step 1: Write the failing smoke test for publish benchmarking**

```cpp
TEST(KvBenchIntegrationTest, PublishBenchmarkCompletesSingleSizeSweep) {
    const auto cfg = zerokv::Config::builder().set_transport("tcp").build();

    auto server = zerokv::kv::KVServer::create(cfg);
    ASSERT_TRUE(server->start(zerokv::kv::ServerConfig{"127.0.0.1:0"}).ok());

    auto node = zerokv::kv::KVNode::create(cfg);
    ASSERT_TRUE(node->start(zerokv::kv::NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "bench-publish-node",
    }).ok());

    const std::string key = "bench-publish-4096-0";
    std::vector<std::byte> payload(4096, std::byte{0x5a});
    auto publish = node->publish(key, payload.data(), payload.size());
    ASSERT_TRUE(publish.status().ok());
    publish.get();

    auto metrics = node->last_publish_metrics();
    ASSERT_TRUE(metrics.has_value());
    EXPECT_GT(metrics->total_us, 0u);

    auto unpublish = node->unpublish(key);
    ASSERT_TRUE(unpublish.status().ok());
    unpublish.get();

    node->stop();
    server->stop();
}
```

- [ ] **Step 2: Add publish benchmark row structures and table rendering**

```cpp
struct PublishBenchRow {
    uint64_t size_bytes = 0;
    uint64_t iterations = 0;
    double avg_total_us = 0.0;
    double avg_prepare_us = 0.0;
    double avg_pack_rkey_us = 0.0;
    double avg_put_meta_rpc_us = 0.0;
    double throughput_mbps = 0.0;
};

std::string render_publish_rows(const std::vector<PublishBenchRow>& rows);
```

- [ ] **Step 3: Implement `bench-publish` loop**

```cpp
std::vector<detail::PublishBenchRow> rows;
for (const auto size_bytes : sizes.value()) {
    const auto iterations = detail::derive_iterations(size_bytes, explicit_iters, total_bytes);
    std::vector<std::byte> payload(size_bytes, std::byte{0x5a});

    uint64_t total_sum = 0;
    uint64_t prepare_sum = 0;
    uint64_t pack_sum = 0;
    uint64_t rpc_sum = 0;

    for (uint64_t i = 0; i < iterations; ++i) {
        const auto key = "bench-publish-" + std::to_string(size_bytes) + "-" + std::to_string(i);
        auto publish = node->publish(key, payload.data(), payload.size());
        if (!publish.status().ok()) {
            std::cerr << "publish benchmark failed: size=" << size_bytes
                      << " iter=" << i << " error=" << publish.status().message() << "\n";
            return 1;
        }
        publish.get();

        const auto metrics = node->last_publish_metrics();
        if (!metrics.has_value() || !metrics->ok) {
            std::cerr << "missing publish metrics for size=" << size_bytes
                      << " iter=" << i << "\n";
            return 1;
        }

        total_sum += metrics->total_us;
        prepare_sum += metrics->prepare_region_us;
        pack_sum += metrics->pack_rkey_us;
        rpc_sum += metrics->put_meta_rpc_us;

        auto unpublish = node->unpublish(key);
        if (!unpublish.status().ok()) {
            std::cerr << "unpublish failed: " << unpublish.status().message() << "\n";
            return 1;
        }
        unpublish.get();
    }

    rows.push_back(detail::PublishBenchRow{
        .size_bytes = size_bytes,
        .iterations = iterations,
        .avg_total_us = static_cast<double>(total_sum) / static_cast<double>(iterations),
        .avg_prepare_us = static_cast<double>(prepare_sum) / static_cast<double>(iterations),
        .avg_pack_rkey_us = static_cast<double>(pack_sum) / static_cast<double>(iterations),
        .avg_put_meta_rpc_us = static_cast<double>(rpc_sum) / static_cast<double>(iterations),
        .throughput_mbps = detail::throughput_mb_per_sec(size_bytes,
            static_cast<double>(total_sum) / static_cast<double>(iterations)),
    });
}
std::cout << detail::render_publish_rows(rows);
```

- [ ] **Step 4: Run the publish benchmark smoke test**

Run:

```bash
cmake --build build --target kv_bench test_kv_bench -j4
cd build && ctest -R IntegrationKvBench --output-on-failure
./kv_bench --mode server --listen 127.0.0.1:17000 --transport tcp &
SERVER_PID=$!
./kv_bench --mode bench-publish --server-addr 127.0.0.1:17000 --data-addr 127.0.0.1:0 --node-id bench-pub --sizes 4K --iters 2 --transport tcp
kill $SERVER_PID
```

Expected:

- output contains one publish table row for `4KiB`
- the command exits `0`

- [ ] **Step 5: Commit**

```bash
git add examples/kv_bench.cpp src/kv/bench_utils.h src/kv/bench_utils.cpp tests/integration/test_kv_bench.cpp
git commit -m "Add KV publish benchmark mode"
```

### Task 4: Implement `bench-fetch`

**Files:**
- Modify: `examples/kv_bench.cpp`
- Modify: `src/kv/bench_utils.h`
- Modify: `src/kv/bench_utils.cpp`
- Test: `tests/integration/test_kv_bench.cpp`

- [ ] **Step 1: Write the failing smoke test for fetch benchmarking**

```cpp
TEST(KvBenchIntegrationTest, FetchBenchmarkReadsStableOwnerKeys) {
    const auto cfg = zerokv::Config::builder().set_transport("tcp").build();

    auto server = zerokv::kv::KVServer::create(cfg);
    ASSERT_TRUE(server->start(zerokv::kv::ServerConfig{"127.0.0.1:0"}).ok());

    auto owner = zerokv::kv::KVNode::create(cfg);
    ASSERT_TRUE(owner->start(zerokv::kv::NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "bench-owner",
    }).ok());

    std::vector<std::byte> payload(4096, std::byte{0x42});
    auto publish = owner->publish("bench-fetch-4096", payload.data(), payload.size());
    ASSERT_TRUE(publish.status().ok());
    publish.get();

    auto reader = zerokv::kv::KVNode::create(cfg);
    ASSERT_TRUE(reader->start(zerokv::kv::NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "bench-reader",
    }).ok());

    auto fetch = reader->fetch("bench-fetch-4096");
    ASSERT_TRUE(fetch.status().ok());
    auto result = fetch.get();
    EXPECT_EQ(result.data.size(), payload.size());

    auto metrics = reader->last_fetch_metrics();
    ASSERT_TRUE(metrics.has_value());
    EXPECT_GT(metrics->total_us, 0u);

    reader->stop();
    owner->stop();
    server->stop();
}
```

- [ ] **Step 2: Add fetch benchmark row structures and rendering**

```cpp
struct FetchBenchRow {
    uint64_t size_bytes = 0;
    uint64_t iterations = 0;
    double avg_total_us = 0.0;
    double avg_prepare_us = 0.0;
    double avg_get_meta_rpc_us = 0.0;
    double avg_peer_connect_us = 0.0;
    double avg_rdma_prepare_us = 0.0;
    double avg_rdma_get_us = 0.0;
    double throughput_mbps = 0.0;
};

std::string render_fetch_rows(const std::vector<FetchBenchRow>& rows);
double throughput_mb_per_sec(uint64_t size_bytes, double avg_total_us);
```

- [ ] **Step 3: Implement `bench-fetch` loop**

```cpp
std::vector<detail::FetchBenchRow> rows;
for (const auto size_bytes : sizes.value()) {
    const auto iterations = detail::derive_iterations(size_bytes, explicit_iters, total_bytes);
    const auto key = "bench-fetch-" + std::to_string(size_bytes);

    uint64_t total_sum = 0;
    uint64_t prepare_sum = 0;
    uint64_t meta_sum = 0;
    uint64_t connect_sum = 0;
    uint64_t rdma_prepare_sum = 0;
    uint64_t rdma_get_sum = 0;

    for (uint64_t i = 0; i < iterations; ++i) {
        auto fetch = node->fetch(key);
        if (!fetch.status().ok()) {
            std::cerr << "fetch benchmark failed: size=" << size_bytes
                      << " iter=" << i << " error=" << fetch.status().message() << "\n";
            return 1;
        }
        const auto result = fetch.get();
        if (result.data.size() != size_bytes) {
            std::cerr << "fetch benchmark size mismatch: expected=" << size_bytes
                      << " actual=" << result.data.size() << "\n";
            return 1;
        }

        const auto metrics = node->last_fetch_metrics();
        if (!metrics.has_value() || !metrics->ok) {
            std::cerr << "missing fetch metrics for size=" << size_bytes
                      << " iter=" << i << "\n";
            return 1;
        }

        total_sum += metrics->total_us;
        prepare_sum += metrics->local_buffer_prepare_us;
        meta_sum += metrics->get_meta_rpc_us;
        connect_sum += metrics->peer_connect_us;
        rdma_prepare_sum += metrics->rdma_prepare_us;
        rdma_get_sum += metrics->rdma_get_us;
    }

    rows.push_back(detail::FetchBenchRow{
        .size_bytes = size_bytes,
        .iterations = iterations,
        .avg_total_us = static_cast<double>(total_sum) / static_cast<double>(iterations),
        .avg_prepare_us = static_cast<double>(prepare_sum) / static_cast<double>(iterations),
        .avg_get_meta_rpc_us = static_cast<double>(meta_sum) / static_cast<double>(iterations),
        .avg_peer_connect_us = static_cast<double>(connect_sum) / static_cast<double>(iterations),
        .avg_rdma_prepare_us = static_cast<double>(rdma_prepare_sum) / static_cast<double>(iterations),
        .avg_rdma_get_us = static_cast<double>(rdma_get_sum) / static_cast<double>(iterations),
        .throughput_mbps = detail::throughput_mb_per_sec(size_bytes,
            static_cast<double>(total_sum) / static_cast<double>(iterations)),
    });
}
std::cout << detail::render_fetch_rows(rows);
```

- [ ] **Step 4: Add owner sanity check using `--owner-node-id`**

```cpp
if (!expected_owner_node_id.empty() && result.owner_node_id != expected_owner_node_id) {
    std::cerr << "fetch benchmark owner mismatch: expected=" << expected_owner_node_id
              << " actual=" << result.owner_node_id << "\n";
    return 1;
}
```

- [ ] **Step 5: Run the fetch smoke path**

Run:

```bash
cmake --build build --target kv_bench test_kv_bench -j4
cd build && ctest -R IntegrationKvBench --output-on-failure
./kv_bench --mode server --listen 127.0.0.1:17010 --transport tcp &
SERVER_PID=$!
./kv_bench --mode hold-owner --server-addr 127.0.0.1:17010 --data-addr 127.0.0.1:0 --node-id owner --sizes 4K --transport tcp &
OWNER_PID=$!
sleep 1
./kv_bench --mode bench-fetch --server-addr 127.0.0.1:17010 --data-addr 127.0.0.1:0 --node-id bench-fetch --owner-node-id owner --sizes 4K --iters 2 --transport tcp
kill $OWNER_PID
kill $SERVER_PID
```

Expected:

- output contains one fetch table row for `4KiB`
- the command exits `0`

- [ ] **Step 6: Commit**

```bash
git add examples/kv_bench.cpp src/kv/bench_utils.h src/kv/bench_utils.cpp tests/integration/test_kv_bench.cpp
git commit -m "Add KV fetch benchmark mode"
```

### Task 5: Final Verification and Docs

**Files:**
- Modify: `docs/reports/zerokv-rdma-kv-mvp.md`
- Modify: `examples/kv_bench.cpp`
- Test: existing benchmark and KV test targets

- [ ] **Step 1: Add usage notes to the MVP report**

```md
### KV Benchmark

Use `kv_bench` for publish/fetch size-sweep benchmarking:

```bash
./kv_bench --mode server --listen <server_ip>:15000 --transport rdma
./kv_bench --mode hold-owner --server-addr <server_ip>:15000 --data-addr <owner_ip>:0 --node-id owner --transport rdma
./kv_bench --mode bench-fetch --server-addr <server_ip>:15000 --data-addr <client_ip>:0 --node-id bench-fetch --owner-node-id owner --transport rdma --total-bytes 1G
./kv_bench --mode bench-publish --server-addr <server_ip>:15000 --data-addr <client_ip>:0 --node-id bench-publish --transport rdma --total-bytes 1G
```
```

- [ ] **Step 2: Run the full relevant verification set**

Run:

```bash
cmake --build build --target kv_bench test_kv_bench test_kv_node test_kv_server -j4
cd build && ctest -R "UnitKvBench|IntegrationKvBench|IntegrationKvNode|IntegrationKvServer|UnitKvMetadataStore" --output-on-failure
```

Expected:

- all listed tests pass

- [ ] **Step 3: Commit**

```bash
git add docs/reports/zerokv-rdma-kv-mvp.md examples/kv_bench.cpp
git commit -m "Document KV benchmark workflow"
```
