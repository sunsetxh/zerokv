# MessageKV Size Sweep Demo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend `examples/message_kv_demo.cpp` into a two-node multi-round size-sweep demo where `RANK1` sends 4 messages per round in parallel and `RANK0` receives the matching 4 messages before advancing to the next size.

**Architecture:** Keep `message_kv_demo` as the only example binary and add a configurable `--sizes` loop around the existing two-node scenario. Receiver stays simple at the CLI level (`--messages`, no receiver thread knob) and internally uses one `MessageKV` instance per in-flight receive to avoid wrapper serialization. Output is one sender summary line and one receiver summary line per round, with deterministic round/key derivation shared by both ranks.

**Tech Stack:** C++20, existing `zerokv::MessageKV`, existing size parsing helpers from KV benchmark code, GoogleTest integration tests, README example documentation.

---

## File Map

- Modify: `examples/message_kv_demo.cpp`
  - Add `--sizes`
  - Add per-round size loop
  - Build deterministic round keys from raw byte sizes
  - Generate exact-size payloads
  - Print `SEND_ROUND` / `RECV_ROUND`
- Modify: `README.md`
  - Update MessageKV demo section to document `--sizes`, default sweep, memory footprint, and example commands
- Create: `tests/integration/test_message_kv_demo.cpp`
  - Add focused formatting/helper tests around key derivation, size parsing, and round summaries
- Modify: `CMakeLists.txt`
  - Add the new integration test target if needed

### Task 1: Add Demo Helper Tests First

**Files:**
- Create: `tests/integration/test_message_kv_demo.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/integration/test_message_kv_demo.cpp`

- [ ] **Step 1: Write the failing test file for size parsing and key derivation**

Create `tests/integration/test_message_kv_demo.cpp` with:

```cpp
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace zerokv::examples::message_kv_demo {
std::vector<size_t> parse_sizes_csv(const std::string& csv);
std::string make_round_key(size_t round_index, size_t size_bytes, size_t thread_index);
std::string make_payload(size_t round_index, size_t size_bytes, size_t thread_index);
}  // namespace zerokv::examples::message_kv_demo

TEST(MessageKvDemoHelpersTest, ParsesDefaultLikeSizeCsv) {
    auto sizes = zerokv::examples::message_kv_demo::parse_sizes_csv("1K,64K,1M");
    ASSERT_EQ(sizes.size(), 3u);
    EXPECT_EQ(sizes[0], 1024u);
    EXPECT_EQ(sizes[1], 64u * 1024u);
    EXPECT_EQ(sizes[2], 1024u * 1024u);
}

TEST(MessageKvDemoHelpersTest, RoundKeyUsesRawByteSize) {
    EXPECT_EQ(
        zerokv::examples::message_kv_demo::make_round_key(2, 1048576, 3),
        "msg-round2-size1048576-thread3");
}

TEST(MessageKvDemoHelpersTest, PayloadHasExactRequestedLength) {
    auto payload = zerokv::examples::message_kv_demo::make_payload(1, 1024, 2);
    EXPECT_EQ(payload.size(), 1024u);
    EXPECT_TRUE(payload.rfind("round1-thread2-", 0) == 0);
}
```

- [ ] **Step 2: Wire the new test target and verify it fails**

Add to `CMakeLists.txt` near the other integration test targets:

```cmake
add_executable(test_message_kv_demo
  tests/integration/test_message_kv_demo.cpp)
target_link_libraries(test_message_kv_demo PRIVATE zerokv GTest::gtest_main)
gtest_discover_tests(test_message_kv_demo
  TEST_PREFIX "IntegrationMessageKvDemo."
  DISCOVERY_TIMEOUT 30)
```

Run: `cmake --build build -j4 --target test_message_kv_demo`

Expected: FAIL during compile or link because the helper functions do not exist yet.

- [ ] **Step 3: Add minimal helper implementations in the demo source**

At the top of `examples/message_kv_demo.cpp`, add a named namespace for reusable helpers:

```cpp
namespace zerokv::examples::message_kv_demo {

std::vector<size_t> parse_sizes_csv(const std::string& csv) {
    auto sizes = zerokv::kv::detail::parse_size_list(csv);
    if (sizes.empty()) {
        throw std::invalid_argument("at least one size is required");
    }
    return sizes;
}

std::string make_round_key(size_t round_index, size_t size_bytes, size_t thread_index) {
    return "msg-round" + std::to_string(round_index) + "-size" +
           std::to_string(size_bytes) + "-thread" + std::to_string(thread_index);
}

std::string make_payload(size_t round_index, size_t size_bytes, size_t thread_index) {
    std::string prefix = "round" + std::to_string(round_index) + "-thread" +
                         std::to_string(thread_index) + "-";
    std::string payload(size_bytes, 'x');
    const auto copy = std::min(prefix.size(), payload.size());
    std::memcpy(payload.data(), prefix.data(), copy);
    return payload;
}

}  // namespace zerokv::examples::message_kv_demo
```

If `kv_bench`'s parser is not directly reusable, copy its exact size-token parsing logic into a small local helper in this file instead of inventing a new format.

- [ ] **Step 4: Run the helper tests and make them pass**

Run: `cmake --build build -j4 --target test_message_kv_demo && ./build/test_message_kv_demo`

Expected: PASS with 3 passing tests.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt examples/message_kv_demo.cpp tests/integration/test_message_kv_demo.cpp
git commit -m "Add MessageKV demo helper tests"
```

### Task 2: Add `--sizes` and Per-Round Sender Sweep

**Files:**
- Modify: `examples/message_kv_demo.cpp`
- Test: `tests/integration/test_message_kv_demo.cpp`

- [ ] **Step 1: Add failing tests for sender round formatting**

Append to `tests/integration/test_message_kv_demo.cpp`:

```cpp
namespace zerokv::examples::message_kv_demo {
std::string render_send_round_summary(size_t round_index,
                                      size_t size_bytes,
                                      size_t messages,
                                      uint64_t send_total_us,
                                      uint64_t max_thread_send_us,
                                      size_t total_bytes);
}

TEST(MessageKvDemoHelpersTest, SendRoundSummaryUsesConfiguredSizeAndCounts) {
    auto line = zerokv::examples::message_kv_demo::render_send_round_summary(
        3, 65536, 4, 1200, 500, 262144);
    EXPECT_NE(line.find("SEND_ROUND"), std::string::npos);
    EXPECT_NE(line.find("round=3"), std::string::npos);
    EXPECT_NE(line.find("size=65536"), std::string::npos);
    EXPECT_NE(line.find("messages=4"), std::string::npos);
    EXPECT_NE(line.find("total_bytes=262144"), std::string::npos);
}
```

- [ ] **Step 2: Run the sender-format test to verify it fails**

Run: `cmake --build build -j4 --target test_message_kv_demo && ./build/test_message_kv_demo --gtest_filter='MessageKvDemoHelpersTest.SendRoundSummaryUsesConfiguredSizeAndCounts'`

Expected: FAIL because `render_send_round_summary` does not exist yet.

- [ ] **Step 3: Implement sender round loop and summary rendering**

In `examples/message_kv_demo.cpp`:

1. Extend `Args`:

```cpp
std::string sizes_csv = "1K,64K,1M,4M,16M,32M,64M,128M";
```

2. Parse `--sizes` in `parse_args`.

3. Add sender summary renderer:

```cpp
std::string render_send_round_summary(size_t round_index,
                                      size_t size_bytes,
                                      size_t messages,
                                      uint64_t send_total_us,
                                      uint64_t max_thread_send_us,
                                      size_t total_bytes) {
    std::ostringstream os;
    os << "SEND_ROUND round=" << round_index
       << " size=" << size_bytes
       << " messages=" << messages
       << " send_total_us=" << send_total_us
       << " max_thread_send_us=" << max_thread_send_us
       << " total_bytes=" << total_bytes
       << " throughput_MiBps=" << throughput_mib_per_sec(total_bytes, send_total_us);
    return os.str();
}
```

4. Rewrite `run_rank1` to:
   - parse `sizes`
   - for each round:
     - generate 4 keys via `make_round_key(round, size, thread)`
     - generate 4 exact-size payloads
     - spawn `args.threads` sender threads
     - join all threads
     - print exactly one `SEND_ROUND ...` line for that round

Keep existing per-thread `SEND_OK ... send_us=...` lines.

- [ ] **Step 4: Run the helper tests and one local sender-only smoke**

Run:

```bash
cmake --build build -j4 --target test_message_kv_demo message_kv_demo
./build/test_message_kv_demo
./build/message_kv_demo --role rank1 --server-addr 127.0.0.1:9 --data-addr 127.0.0.1:0 --node-id rank1 --threads 4 --sizes 1K
```

Expected:
- helper tests PASS
- sender run FAILS with connection error, but argument parsing accepts `--sizes 1K`

- [ ] **Step 5: Commit**

```bash
git add examples/message_kv_demo.cpp tests/integration/test_message_kv_demo.cpp
git commit -m "Add MessageKV sender size sweep rounds"
```

### Task 3: Add Per-Round Receiver Sweep and Compact Validation Output

**Files:**
- Modify: `examples/message_kv_demo.cpp`
- Test: `tests/integration/test_message_kv_demo.cpp`

- [ ] **Step 1: Add failing tests for receiver round formatting**

Append:

```cpp
namespace zerokv::examples::message_kv_demo {
std::string render_recv_round_summary(size_t round_index,
                                      size_t size_bytes,
                                      size_t completed,
                                      size_t failed,
                                      size_t timed_out,
                                      bool completed_all,
                                      uint64_t recv_total_us,
                                      size_t total_bytes);
}

TEST(MessageKvDemoHelpersTest, RecvRoundSummaryUsesConfiguredSizeAndCounts) {
    auto line = zerokv::examples::message_kv_demo::render_recv_round_summary(
        1, 1024, 4, 0, 0, true, 900, 4096);
    EXPECT_NE(line.find("RECV_ROUND"), std::string::npos);
    EXPECT_NE(line.find("round=1"), std::string::npos);
    EXPECT_NE(line.find("size=1024"), std::string::npos);
    EXPECT_NE(line.find("completed=4"), std::string::npos);
    EXPECT_NE(line.find("completed_all=1"), std::string::npos);
}
```

- [ ] **Step 2: Run the receiver-format test to verify it fails**

Run: `cmake --build build -j4 --target test_message_kv_demo && ./build/test_message_kv_demo --gtest_filter='MessageKvDemoHelpersTest.RecvRoundSummaryUsesConfiguredSizeAndCounts'`

Expected: FAIL because `render_recv_round_summary` does not exist yet.

- [ ] **Step 3: Implement per-round receiver loop**

In `examples/message_kv_demo.cpp`, rewrite `run_rank0` to:

- parse `sizes`
- for each round:
  - build 4 expected keys with `make_round_key(round, size, thread)`
  - build 4 expected payloads with `make_payload(round, size, thread)`
  - allocate one shared `MemoryRegion` sized `messages * size`
  - start one `MessageKV` receiver instance per message
  - run `recv()` concurrently for each key into non-overlapping offsets
  - collect `completed/failed/timed_out`
  - print one `RECV_ROUND ...` line
  - print compact `RECV_OK ... bytes=<size> preview=<prefix>` detail lines
  - sleep `post_recv_wait_ms`

Add:

```cpp
std::string render_recv_round_summary(...);
std::string make_preview(const std::string& payload, size_t preview_len = 32);
```

The preview helper should truncate long payloads rather than dumping all bytes.

- [ ] **Step 4: Run helper tests and a same-host two-process smoke**

Run:

```bash
cmake --build build -j4 --target test_message_kv_demo message_kv_demo
./build/test_message_kv_demo
./build/message_kv_demo --role rank0 --listen 127.0.0.1:16050 --data-addr 127.0.0.1:0 --node-id rank0 --messages 4 --sizes 1K,64K --timeout-ms 5000 --transport tcp > /tmp/mk-rank0.log 2>&1 &
./build/message_kv_demo --role rank1 --server-addr 127.0.0.1:16050 --data-addr 127.0.0.1:0 --node-id rank1 --threads 4 --sizes 1K,64K --transport tcp
cat /tmp/mk-rank0.log
```

Expected:
- two `SEND_ROUND` lines
- two `RECV_ROUND` lines
- all rounds `completed_all=1`

- [ ] **Step 5: Commit**

```bash
git add examples/message_kv_demo.cpp tests/integration/test_message_kv_demo.cpp
git commit -m "Add MessageKV receiver size sweep rounds"
```

### Task 4: Update README and Real-Environment Guidance

**Files:**
- Modify: `README.md`
- Test: manual doc sanity check

- [ ] **Step 1: Update the MessageKV demo section**

Replace the current single-round command block and prose with:

- `--sizes` flag documentation
- default sweep list
- note that `RANK0` runs `KVServer + receiver` in one process
- note that `RANK1` sends 4 messages per round from 4 threads
- note that default max receive buffer for the last round is roughly `4 * 128MiB = 512MiB`

Use concrete command examples:

```bash
./build/message_kv_demo \
  --role rank0 \
  --listen 10.0.0.1:15000 \
  --data-addr 10.0.0.1:0 \
  --node-id rank0-receiver \
  --messages 4 \
  --sizes 1K,64K,1M,4M,16M,32M,64M,128M \
  --timeout-ms 30000 \
  --transport rdma
```

```bash
./build/message_kv_demo \
  --role rank1 \
  --server-addr 10.0.0.1:15000 \
  --data-addr 10.0.0.2:0 \
  --node-id rank1-sender \
  --threads 4 \
  --sizes 1K,64K,1M,4M,16M,32M,64M,128M \
  --transport rdma
```

- [ ] **Step 2: Add warmup note to the docs**

Add one short note in README:

- first round may include cold-start costs
- if users care about steady-state, start with a small size or run a warmup pass first

- [ ] **Step 3: Read the updated README section for consistency**

Run: `sed -n '170,280p' README.md`

Expected:
- command lines match actual CLI flags
- no stale `--cleanup-wait-ms`
- no stale receiver `--threads`

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "Document MessageKV size sweep demo"
```

### Task 5: VM Validation of the New Round Model

**Files:**
- Modify: none
- Test: built binaries on VM1/VM2

- [ ] **Step 1: Build the demo on VM1 and VM2**

Run on both VMs:

```bash
cd ~/zerokv-task
cmake --build build -j4 --target message_kv_demo
```

Expected: PASS on both machines.

- [ ] **Step 2: Run a two-node TCP smoke with two sizes**

On VM1:

```bash
UCX_NET_DEVICES=enp0s1 ./build/message_kv_demo \
  --role rank0 \
  --listen 10.0.0.1:16060 \
  --data-addr 10.0.0.1:0 \
  --node-id rank0-receiver \
  --messages 4 \
  --sizes 1K,64K \
  --timeout-ms 30000 \
  --post-recv-wait-ms 1000 \
  --transport tcp
```

On VM2:

```bash
UCX_NET_DEVICES=enp0s1 ./build/message_kv_demo \
  --role rank1 \
  --server-addr 10.0.0.1:16060 \
  --data-addr 10.0.0.2:0 \
  --node-id rank1-sender \
  --threads 4 \
  --sizes 1K,64K \
  --transport tcp
```

Expected:
- VM2 prints two `SEND_ROUND` lines
- VM1 prints two `RECV_ROUND` lines
- both rounds complete with `completed_all=1`

- [ ] **Step 3: Run a two-node RDMA smoke with one small and one medium size**

On VM1:

```bash
UCX_NET_DEVICES=enp0s1 ./build/message_kv_demo \
  --role rank0 \
  --listen 10.0.0.1:16061 \
  --data-addr 10.0.0.1:0 \
  --node-id rank0-receiver \
  --messages 4 \
  --sizes 1K,1M \
  --timeout-ms 30000 \
  --post-recv-wait-ms 1000 \
  --transport rdma
```

On VM2:

```bash
UCX_NET_DEVICES=enp0s1 ./build/message_kv_demo \
  --role rank1 \
  --server-addr 10.0.0.1:16061 \
  --data-addr 10.0.0.2:0 \
  --node-id rank1-sender \
  --threads 4 \
  --sizes 1K,1M \
  --transport rdma
```

Expected:
- both rounds complete
- summary lines use raw byte sizes (`1024`, `1048576`)

- [ ] **Step 4: Commit the verified final state**

```bash
git add examples/message_kv_demo.cpp README.md tests/integration/test_message_kv_demo.cpp CMakeLists.txt
git commit -m "Add MessageKV size sweep demo"
```

## Self-Review

- Spec coverage:
  - `--sizes` support: Task 1 + Task 2
  - per-round sender join model: Task 2
  - per-round receiver model: Task 3
  - README updates: Task 4
  - VM verification: Task 5
- Placeholder scan:
  - no `TODO`, `TBD`, or implicit “write tests later” steps remain
- Type consistency:
  - helper names are defined once and reused consistently:
    - `parse_sizes_csv`
    - `make_round_key`
    - `make_payload`
    - `render_send_round_summary`
    - `render_recv_round_summary`
