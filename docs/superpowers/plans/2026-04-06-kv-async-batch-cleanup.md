# KV Async Batch Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Improve sender-side async completion behavior by changing `KV`'s single cleanup thread from FIFO serial ack waiting to batch wait-any cleanup, without changing public API or synchronous sender semantics.

**Architecture:** Keep one cleanup thread per `KV`. `send_async()` and `send_region_async()` still publish immediately and enqueue pending sends. The cleanup thread now drains newly enqueued sends into an active map keyed by `ack_key`, waits on `wait_for_any_subscription_event(...)`, resolves whichever ack arrives first, performs unsubscribe + unpublish, fulfills the matching future, and repeats until shutdown.

**Tech Stack:** C++20, current `zerokv::transport::Future<void>` / `Promise<void>`, `zerokv::core::KVNode`, GoogleTest integration tests, VM1/VM2 TCP/RDMA validation.

---

## File Map

- Modify: `src/kv.cpp`
  - batch cleanup data structures and wait-any cleanup loop
- Modify: `tests/integration/test_message_kv.cpp`
  - batch async cleanup tests
- Optional validation only: `examples/message_kv_demo.cpp`
  - no required logic change; reuse `--send-mode async` for validation
- Optional docs after validation: `docs/reports/current-zerokv-implementation.md`

## Task 1: Add failing tests for batched async cleanup behavior

**Files:**
- Modify: `tests/integration/test_message_kv.cpp`

- [ ] **Step 1: Add a completion-order test**

Add a test that issues multiple async sends and proves a later-enqueued send can
complete first if its ack arrives first.

Suggested shape:

```cpp
TEST_F(KvIntegrationTest, AsyncSendCleanupCompletesByAckArrivalOrder) {
    auto sender = zerokv::KV::create(cfg_);
    auto receiver = zerokv::KV::create(cfg_);
    sender->start(sender_cfg_);
    receiver->start(receiver_cfg_);

    auto region_a = sender->allocate_send_region(8);
    auto region_b = sender->allocate_send_region(8);
    std::memcpy(region_a->address(), "payload-a", 8);
    std::memcpy(region_b->address(), "payload-b", 8);

    auto future_a = sender->send_region_async("async-a", region_a, 8);
    auto future_b = sender->send_region_async("async-b", region_b, 8);

    auto recv_region = zerokv::transport::MemoryRegion::allocate(ctx_, 16);
    receiver->recv("async-b", recv_region, 8, 0, std::chrono::seconds(5));

    EXPECT_TRUE(future_b.wait_for(std::chrono::milliseconds(500)));
    EXPECT_FALSE(future_a.wait_for(std::chrono::milliseconds(50)));

    receiver->recv("async-a", recv_region, 8, 8, std::chrono::seconds(5));
    future_a.get();
    future_b.get();
}
```

- [ ] **Step 2: Add a stop-with-active-batch test if current coverage is too narrow**

If existing stop-before-ack coverage only exercises one pending send, add a
multi-send variant and require both pending futures to fail on stop.

- [ ] **Step 3: Run focused tests and confirm the new completion-order test fails**

Run:

```bash
cmake --build build -j4 --target test_message_kv
./build/test_message_kv --gtest_filter="KvIntegrationTest.AsyncSendCleanupCompletesByAckArrivalOrder:KvIntegrationTest.SendRegionAsyncFailsIfStoppedBeforeAck"
```

Expected: the new completion-order test fails under the current FIFO cleanup
loop.

- [ ] **Step 4: Commit**

```bash
git add tests/integration/test_message_kv.cpp
git commit -m "Add KV async batch cleanup tests"
```

## Task 2: Replace FIFO cleanup with batch wait-any cleanup

**Files:**
- Modify: `src/kv.cpp`

- [ ] **Step 1: Refactor pending-send storage**

Keep the producer queue for newly enqueued items, but add an active map keyed by
`ack_key`, for example:

```cpp
std::unordered_map<std::string, PendingAsyncSend> active_async_sends;
```

Each `PendingAsyncSend` should still own:

- `message_key`
- `ack_key`
- `Promise<void>`
- `subscribed`

- [ ] **Step 2: Split cleanup loop into three phases**

Refactor `cleanup_loop()` into small helpers:

- `drain_pending_async_sends_locked()`
- `wait_for_ready_async_ack(...)`
- `complete_ready_async_send(...)`

Target behavior:

1. drain newly enqueued sends into the active map
2. if no active sends, wait on `cleanup_cv`
3. otherwise wait for any ack among active ack keys
4. complete whichever ack keys are ready

- [ ] **Step 3: Implement wait-any ack detection**

Use:

```cpp
node->wait_for_any_subscription_event(active_ack_keys, std::chrono::milliseconds(50))
```

If an event is returned, mark that `ack_key` ready.

After each wait slice, run fallback `wait_for_key(ack_key, 1ms)` checks over the
still-active keys as needed.

- [ ] **Step 4: Complete each ready send independently**

For each ready `ack_key`:

1. unsubscribe `ack_key`
2. unpublish `message_key`
3. `promise.set_value()` or `set_error(...)`
4. remove the item from the active map

Do not block completion of one ready send behind another not-yet-ready send.

- [ ] **Step 5: Preserve shutdown behavior**

Ensure `stop()` still:

- signals the cleanup thread
- joins it
- fails both queued and active pending futures with `kConnectionReset`

- [ ] **Step 6: Add or refine trace logging**

Keep trace lines concise and batch-oriented, for example:

- `MESSAGE_KV_ASYNC_BATCH_DRAIN active=... queued=...`
- `MESSAGE_KV_ASYNC_BATCH_WAIT_ANY active=...`
- `MESSAGE_KV_ASYNC_BATCH_COMPLETE key=... status=...`

- [ ] **Step 7: Run focused tests and confirm they pass**

Run:

```bash
cmake --build build -j4 --target test_message_kv
./build/test_message_kv --gtest_filter="KvIntegrationTest.AsyncSendCleanupCompletesByAckArrivalOrder:KvIntegrationTest.SendRegionAsyncCompletesAfterAckAndCleanup:KvIntegrationTest.SendRegionAsyncFailsIfStoppedBeforeAck:KvIntegrationTest.SendRegionStillWaitsForAckBeforeReturning"
```

Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add src/kv.cpp
git commit -m "Batch KV async cleanup over wait-any"
```

## Task 3: Validate on VM1/VM2 and record the async sender effect

**Files:**
- No required code changes
- Optional docs update after validation

- [ ] **Step 1: VM1 focused test validation**

Run:

```bash
cmake --build build -j4 --target test_message_kv test_message_kv_demo
./build/test_message_kv --gtest_filter="KvIntegrationTest.AsyncSendCleanupCompletesByAckArrivalOrder:KvIntegrationTest.SendRegionAsyncCompletesAfterAckAndCleanup:KvIntegrationTest.SendRegionAsyncFailsIfStoppedBeforeAck:KvIntegrationTest.SendRegionStillWaitsForAckBeforeReturning"
./build/test_message_kv_demo
```

- [ ] **Step 2: VM1/VM2 async demo comparison**

Run `message_kv_demo` on TCP and RDMA with:

- `--send-mode async`
- `--sizes 64K,1M`
- `--warmup-rounds 1`

Compare sender-side:

- per-thread `send_us`
- `SEND_ROUND total_us`

against the current async baseline.

- [ ] **Step 3: Confirm expected outcome**

Expected:

- sender thread return latency stays low
- async future completion becomes less serialized under multiple in-flight sends
- no recv regression

- [ ] **Step 4: Commit validation-only notes if and only if docs are updated**

If you update the report, use a separate commit:

```bash
git add docs/reports/current-zerokv-implementation.md
git commit -m "Document KV async batch cleanup results"
```
