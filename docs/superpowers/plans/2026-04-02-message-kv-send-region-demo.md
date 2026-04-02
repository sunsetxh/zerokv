# MessageKV Send Region Demo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Switch `examples/message_kv_demo.cpp` sender rounds to reusable per-thread `MemoryRegion` buffers and `send_region()` while preserving the existing public MessageKV API and output format.

**Architecture:** Keep `MessageKV::send()` and `MessageKV::send_region()` unchanged. Only the demo sender path changes: each sender thread creates one `MessageKV` instance and one reusable send region sized to the largest configured payload, rewrites the region contents each round, and sends via `send_region()`. This preserves all current semantics while removing repeated region allocation/registration from the measured loop.

**Tech Stack:** C++20, ZeroKV `MessageKV`, `MemoryRegion`, GoogleTest, existing demo/README workflow

---

## File Map

- Modify: `examples/message_kv_demo.cpp`
  - Change rank1 sender loop to allocate one reusable send region per thread and call `send_region()`.
- Modify: `README.md`
  - Update the MessageKV demo section if any sender description becomes stale after the switch to `send_region()`.
- Verify: `tests/integration/test_message_kv_demo.cpp`
  - Existing helper tests should continue to pass unchanged.
- Verify: `tests/integration/test_message_kv.cpp`
  - Existing MessageKV integration tests should continue to pass unchanged.

### Task 1: Update sender rounds to reuse a per-thread send region

**Files:**
- Modify: `examples/message_kv_demo.cpp`
- Test: `tests/integration/test_message_kv_demo.cpp`

- [ ] **Step 1: Inspect the current sender round loop and locate the thread worker lambda**

Run:
```bash
sed -n '420,540p' examples/message_kv_demo.cpp
```
Expected: a sender thread lambda that currently builds a payload string each round and calls `mq->send(key, payload.data(), payload.size())`.

- [ ] **Step 2: Write down the exact code shape to introduce a reusable region per thread**

Use this structure inside each sender thread:

```cpp
auto ctx = zerokv::Context::create(config);
auto region = ctx->allocate_memory_region(max_size_bytes);
auto mq = zerokv::MessageKV::create(config);
mq->start({
    .server_addr = args.server_addr,
    .local_data_addr = args.data_addr,
    .node_id = node_id,
});

for (size_t round_index = 0; round_index < all_rounds; ++round_index) {
    const auto size_bytes = sizes[round_index_for_payload];
    const auto key = zerokv::examples::message_kv_demo::make_round_key(
        measured_round_index, size_bytes, thread_index);
    const auto payload = zerokv::examples::message_kv_demo::make_payload(
        measured_round_index, size_bytes, thread_index);
    std::memcpy(region->address(), payload.data(), size_bytes);
    mq->send_region(key, region, size_bytes);
}
```

Expected: one `Context`, one `MemoryRegion`, one `MessageKV` per sender thread, all reused across warmup and measured rounds.

- [ ] **Step 3: Implement the minimal sender change in `examples/message_kv_demo.cpp`**

Update the sender path so each thread:
- computes `max_size_bytes` once from `sizes`
- allocates exactly one reusable region with that size
- copies each round payload into the front of the region
- calls `send_region(key, region, size_bytes)` instead of `send(...)`

Key implementation constraints:

```cpp
const auto max_size_bytes =
    *std::max_element(sizes.begin(), sizes.end());
```

```cpp
auto ctx = zerokv::Context::create(config);
auto region = ctx->allocate_memory_region(max_size_bytes);
```

```cpp
const auto payload = zerokv::examples::message_kv_demo::make_payload(
    measured_round_index, size_bytes, thread_index);
std::memcpy(region->address(), payload.data(), size_bytes);
mq->send_region(key, region, size_bytes);
```

Do not change:
- sender summary fields
- receiver logic
- key naming
- warmup round behavior

- [ ] **Step 4: Rebuild the demo helper test target**

Run:
```bash
cmake --build build -j4 --target test_message_kv_demo message_kv_demo
```
Expected: build succeeds.

- [ ] **Step 5: Run the existing helper tests**

Run:
```bash
./build/test_message_kv_demo
```
Expected: all existing demo helper tests pass unchanged.

- [ ] **Step 6: Commit the sender change**

```bash
git add examples/message_kv_demo.cpp
git commit -m "Use send_region in MessageKV demo sender"
```

### Task 2: Refresh MessageKV demo documentation if needed

**Files:**
- Modify: `README.md`
- Verify: `examples/message_kv_demo.cpp`

- [ ] **Step 1: Inspect the current README MessageKV demo section**

Run:
```bash
rg -n "message_kv_demo|warmup-rounds|SEND_ROUND|RECV_ROUND" README.md
sed -n '120,240p' README.md
```
Expected: the existing two-node MessageKV demo section and size-sweep notes.

- [ ] **Step 2: Update only stale sender-side wording**

If README currently implies sender rounds call `send(...)`, replace that description with wording that stays true after the change, for example:

```md
Each sender thread reuses a preallocated send buffer and sends one message per round.
```

Do not add new CLI flags or change the example commands.

- [ ] **Step 3: Sanity-check that the docs still match the actual demo CLI**

Run:
```bash
./build/message_kv_demo --help | sed -n '1,80p'
```
Expected: the same public CLI as before (`--role`, `--sizes`, `--warmup-rounds`, `--threads`, `--messages`, `--timeout-ms`, `--transport`).

- [ ] **Step 4: Commit the doc refresh if any text changed**

```bash
git add README.md
if ! git diff --cached --quiet; then
  git commit -m "Refresh MessageKV demo sender docs"
fi
```

### Task 3: Verify steady-state behavior locally and on VM1/VM2

**Files:**
- Verify only: `examples/message_kv_demo.cpp`, `README.md`

- [ ] **Step 1: Run core MessageKV integration smoke locally**

Run:
```bash
./build/test_message_kv --gtest_filter="MessageKvIntegrationTest.RecvCopiesSingleMessageIntoRegion:MessageKvIntegrationTest.SendUnpublishesMessageKeyAfterAck"
```
Expected: both tests pass.

- [ ] **Step 2: Run VM1/VM2 TCP smoke with `1K,1M`**

Run on VM1 (`rank0`):
```bash
UCX_NET_DEVICES=enp0s1 ./build/message_kv_demo \
  --role rank0 \
  --listen 10.0.0.1:16000 \
  --data-addr 10.0.0.1:0 \
  --node-id rank0-receiver \
  --messages 4 \
  --warmup-rounds 1 \
  --sizes 1K,1M \
  --timeout-ms 30000 \
  --post-recv-wait-ms 4000 \
  --transport tcp
```

Run on VM2 (`rank1`):
```bash
UCX_NET_DEVICES=enp0s1 ./build/message_kv_demo \
  --role rank1 \
  --server-addr 10.0.0.1:16000 \
  --data-addr 10.0.0.2:0 \
  --node-id rank1-sender \
  --threads 4 \
  --warmup-rounds 1 \
  --sizes 1K,1M \
  --transport tcp
```
Expected: two measured rounds complete with `SEND_ROUND` and `RECV_ROUND` output, no failures or timeouts.

- [ ] **Step 3: Run VM1/VM2 RDMA smoke with `1K,1M`**

Run on VM1 (`rank0`):
```bash
UCX_NET_DEVICES=rxe0:1 ./build/message_kv_demo \
  --role rank0 \
  --listen 10.0.0.1:16000 \
  --data-addr 10.0.0.1:0 \
  --node-id rank0-receiver \
  --messages 4 \
  --warmup-rounds 1 \
  --sizes 1K,1M \
  --timeout-ms 30000 \
  --post-recv-wait-ms 4000 \
  --transport rdma
```

Run on VM2 (`rank1`):
```bash
UCX_NET_DEVICES=rxe0:1 ./build/message_kv_demo \
  --role rank1 \
  --server-addr 10.0.0.1:16000 \
  --data-addr 10.0.0.2:0 \
  --node-id rank1-sender \
  --threads 4 \
  --warmup-rounds 1 \
  --sizes 1K,1M \
  --transport rdma
```
Expected: two measured rounds complete with `SEND_ROUND` and `RECV_ROUND` output, no failures or timeouts.

- [ ] **Step 4: Compare measured sender-side rounds against the previous `send()` path**

Record the new `SEND_ROUND` lines for `1MiB` and compare them against the prior baseline captured in the notes/report. The expectation is lower sender-side overhead because repeated region allocation/registration is removed.

- [ ] **Step 5: Commit only if code or docs changed during verification cleanup**

```bash
git add examples/message_kv_demo.cpp README.md
if ! git diff --cached --quiet; then
  git commit -m "Verify MessageKV send_region demo path"
fi
```
