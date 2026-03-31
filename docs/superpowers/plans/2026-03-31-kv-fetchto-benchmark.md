# KV FetchTo and Benchmark Metrics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Clarify `fetch_to()` as the zero-copy public fetch path, fix fetch metric accounting, and add a cleaner `bench-fetch-to` benchmark mode.

**Architecture:** Extend `FetchMetrics` with one missing stage (`result_copy_us`), keep `fetch()` as end-to-end convenience API, and add `bench-fetch-to` as a benchmark that reuses a caller-owned destination region. Update docs/TODO to record the multi-NIC findings and the intended zero-copy API guidance.

**Tech Stack:** C++17, CMake, existing KVServer/KVNode APIs, gtest integration tests.

---

### Task 1: Add failing tests for fetch metrics and benchmark output

**Files:**
- Modify: `tests/integration/test_kv_node.cpp`
- Modify: `tests/integration/test_kv_bench.cpp`

- [ ] **Step 1: Write the failing fetch metrics tests**

Add tests that assert:
- `fetch()` records a non-zero `result_copy_us`
- `fetch_to()` leaves `result_copy_us == 0`

- [ ] **Step 2: Run the targeted tests to verify they fail**

Run: `cmake --build build --target test_kv_node test_kv_bench -j4 && ./build/test_kv_node --gtest_filter='KvNodeIntegrationTest.FetchMetricsIncludeResultCopy:KvNodeIntegrationTest.FetchToLeavesResultCopyZero' && ./build/test_kv_bench --gtest_filter='KvBenchIntegrationTest.RenderedTablesUseMiBpsHeader:KvBenchIntegrationTest.FetchToSmoke'`
Expected: FAIL because `result_copy_us` field and `bench-fetch-to` mode/header do not exist yet.

### Task 2: Implement fetch metrics accounting and MiB naming

**Files:**
- Modify: `include/axon/kv.h`
- Modify: `src/kv/node.cpp`
- Modify: `src/python/bindings.cpp`
- Modify: `src/kv/bench_utils.h`
- Modify: `src/kv/bench_utils.cpp`

- [ ] **Step 1: Add the new metric field**

Add `uint64_t result_copy_us = 0;` to `FetchMetrics` and expose it through Python bindings.

- [ ] **Step 2: Record result copy time in `fetch()`**

Measure the `result.data.resize()` + `std::memcpy(...)` section into `result_copy_us`. Keep `fetch_to()` at zero for this field.

- [ ] **Step 3: Rename throughput labels to MiB**

Rename benchmark row fields and printed column labels from `throughput_MBps` to `throughput_MiBps`, keeping the underlying calculation unchanged.

- [ ] **Step 4: Run tests to verify the metrics/header changes pass**

Run: `cmake --build build --target test_kv_node test_kv_bench -j4 && ./build/test_kv_node --gtest_filter='KvNodeIntegrationTest.FetchMetricsIncludeResultCopy:KvNodeIntegrationTest.FetchToLeavesResultCopyZero' && ./build/test_kv_bench --gtest_filter='KvBenchIntegrationTest.RenderedTablesUseMiBpsHeader'`
Expected: PASS.

### Task 3: Add `bench-fetch-to`

**Files:**
- Modify: `examples/kv_bench.cpp`
- Modify: `src/kv/bench_utils.h`
- Modify: `src/kv/bench_utils.cpp`
- Modify: `tests/integration/test_kv_bench.cpp`

- [ ] **Step 1: Write the failing benchmark smoke test**

Add a smoke/integration test that exercises the new `bench-fetch-to` path over a tiny local benchmark setup and checks that at least one row is rendered.

- [ ] **Step 2: Run the new test to verify it fails**

Run: `cmake --build build --target test_kv_bench -j4 && ./build/test_kv_bench --gtest_filter='KvBenchIntegrationTest.FetchToSmoke'`
Expected: FAIL because `bench-fetch-to` is not implemented.

- [ ] **Step 3: Implement `bench-fetch-to`**

In `kv_bench`:
- accept `--mode bench-fetch-to`
- pre-allocate one reusable `MemoryRegion` per size
- call `fetch_to()` in the inner loop
- reuse existing fetch metrics collection/output shape
- print the same fetch table, now with `throughput_MiBps`

- [ ] **Step 4: Run benchmark tests**

Run: `cmake --build build --target kv_bench test_kv_bench -j4 && ./build/test_kv_bench --gtest_filter='KvBenchIntegrationTest.FetchToSmoke:KvBenchIntegrationTest.RenderedTablesUseMiBpsHeader'`
Expected: PASS.

### Task 4: Update docs and TODOs

**Files:**
- Modify: `README.md`
- Modify: `docs/reports/axon-rdma-kv-mvp.md`
- Modify: `docs/reports/todo-cluster-next-steps.md`

- [ ] **Step 1: Document zero-copy fetch positioning**

Update README and MVP report to state:
- `fetch()` is convenience/E2E
- `fetch_to()` is the zero-copy public API for performance-sensitive paths
- `bench-fetch` includes result copy while `bench-fetch-to` is cleaner for data-plane measurement

- [ ] **Step 2: Record multi-NIC follow-up TODO**

Add a TODO entry summarizing:
- UCX multi-rail support exists
- AXON currently registers one `data_addr`
- future work: multi-address registration -> per-key NIC selection -> optional striping

- [ ] **Step 3: Run a focused doc/benchmark sanity check**

Run: `cmake --build build --target kv_bench -j4 && ./build/kv_bench --mode bench-fetch-to --help >/tmp/kv_bench_help.txt 2>&1 || true && rg -n 'fetch_to|throughput_MiBps|multi-NIC|multi-rail|data_addr' README.md docs/reports/axon-rdma-kv-mvp.md docs/reports/todo-cluster-next-steps.md`
Expected: docs mention the new terminology and TODO text.

### Task 5: Final verification and commit

**Files:**
- Modify: all files touched above

- [ ] **Step 1: Run full targeted verification**

Run: `cmake --build build --target kv_bench test_kv_node test_kv_bench -j4 && ctest --test-dir build -R 'IntegrationKvNode|KvBench' --output-on-failure`
Expected: PASS.

- [ ] **Step 2: Commit**

```bash
git add include/axon/kv.h src/kv/node.cpp src/python/bindings.cpp src/kv/bench_utils.h src/kv/bench_utils.cpp examples/kv_bench.cpp tests/integration/test_kv_node.cpp tests/integration/test_kv_bench.cpp README.md docs/reports/axon-rdma-kv-mvp.md docs/reports/todo-cluster-next-steps.md
git commit -m "Improve KV fetch metrics and benchmarks"
```
