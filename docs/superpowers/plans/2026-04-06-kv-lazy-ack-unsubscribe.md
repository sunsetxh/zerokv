# KV Lazy Ack Unsubscribe Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove `unsubscribe(ack_key)` from the sender completion hot path so async and sync sender completion requires only ack observation plus message-key unpublish, while deferring sender ack subscription cleanup to existing sweep/stop paths.

**Architecture:** Keep the public `KV` API unchanged. Extend `KV::Impl` with deferred sender-ack unsubscribe tracking. Async cleanup now observes ack, unpublishes the message key, fulfills the future, and records the ack key for later unsubscribe. Existing sweep points and `stop()` drain those stale sender ack subscriptions best-effort.

**Tech Stack:** C++20, current `zerokv::transport::Future<void>` / `Promise<void>`, `zerokv::core::KVNode`, GoogleTest integration tests, VM1/VM2 TCP/RDMA validation.

---

## File Map

- Modify: `src/kv.cpp`
  - remove hot-path ack unsubscribe from sender cleanup
  - add deferred sender ack unsubscribe state and sweep
- Modify: `tests/integration/test_message_kv.cpp`
  - add regression coverage for deferred sender ack unsubscribe
- Optional docs after validation: `docs/reports/current-zerokv-implementation.md`

## Task 1: Add focused tests for deferred sender ack unsubscribe

**Files:**
- Modify: `tests/integration/test_message_kv.cpp`

- [ ] **Step 1: Add a regression test that async completion no longer depends on immediate unsubscribe**

Add a test that:

1. issues `send_region_async()`
2. receiver consumes and acks
3. future completes success
4. message key is gone from the server

This overlaps existing success coverage, but assert explicitly that the success
criterion is message-key cleanup, not ack unsubscribe timing.

- [ ] **Step 2: Add a stale-sender-ack cleanup test**

Add a test that:

1. completes an async send
2. leaves the sender running
3. triggers a later sweep point (for example another send or explicit stop)
4. confirms no regressions or hangs occur when stale sender ack subscriptions
   are cleaned up lazily

The test does not need to introspect subscription internals if that would
require new API; it can validate via continued correct operation and bounded
stop behavior.

- [ ] **Step 3: Run focused tests and confirm they fail or are incomplete before implementation**

Run:

```bash
cmake --build build -j4 --target test_message_kv
./build/test_message_kv --gtest_filter="KvIntegrationTest.SendRegionAsyncCompletesAfterAckAndCleanup:KvIntegrationTest.AsyncSendCleanupCompletesByAckArrivalOrder:KvIntegrationTest.SendRegionAsyncFailsIfStoppedBeforeAck"
```

If the new test is purely additive and already passes structurally, that is
acceptable; the main purpose is to pin the intended semantics before the code
change.

- [ ] **Step 4: Commit**

```bash
git add tests/integration/test_message_kv.cpp
git commit -m "Add KV lazy ack unsubscribe tests"
```

## Task 2: Remove hot-path unsubscribe and defer sender ack cleanup

**Files:**
- Modify: `src/kv.cpp`

- [ ] **Step 1: Add deferred sender-ack cleanup tracking**

Extend `KV::Impl` with sender-side stale ack subscription tracking, for example:

```cpp
std::vector<std::string> stale_sender_ack_keys;
```

or a deduplicating set if needed.

- [ ] **Step 2: Add sender-ack sweep helper**

Add a helper analogous to receiver ack cleanup, for example:

```cpp
void sweep_sender_ack_unsubscribe_locked();
```

Behavior:

- if node not running or list empty, return
- attempt `node->unsubscribe(ack_key)` for each stale sender ack key
- keep only failures for later retry

- [ ] **Step 3: Integrate sender-ack sweep into existing sweep points**

Update `sweep_cleanup_locked()` so it drains both:

- receiver-owned ack key unpublish cleanup
- sender stale ack subscription cleanup

This ensures deferred unsubscribes are attempted on future sends/recvs and at
stop.

- [ ] **Step 4: Change async cleanup completion order**

In the batch async cleanup loop:

- remove `unsubscribe(ack_key)` from the per-send hot path
- after ack is observed and `unpublish(message_key)` succeeds:
  - fulfill the promise
  - record `ack_key` in `stale_sender_ack_keys`

If `unpublish(message_key)` fails:

- future still fails
- do not claim successful completion

- [ ] **Step 5: Preserve stop semantics**

Ensure `stop()` still:

- stops cleanup thread
- fails queued/active pending futures
- runs `sweep_cleanup_locked()` before node shutdown

Sender stale ack unsubscribe failures during stop remain non-fatal.

- [ ] **Step 6: Adjust trace logging**

Replace hot-path unsubscribe traces with lazy-cleanup traces such as:

- `MESSAGE_KV_ASYNC_DEFER_UNSUBSCRIBE`
- `MESSAGE_KV_SWEEP_ACK_UNSUBSCRIBE`
- `MESSAGE_KV_SWEEP_ACK_UNSUBSCRIBE_FAILED`

- [ ] **Step 7: Run focused tests and confirm they pass**

Run:

```bash
cmake --build build -j4 --target test_message_kv
./build/test_message_kv --gtest_filter="KvIntegrationTest.SendRegionAsyncCompletesAfterAckAndCleanup:KvIntegrationTest.AsyncSendCleanupCompletesByAckArrivalOrder:KvIntegrationTest.SendRegionAsyncFailsIfStoppedBeforeAck:KvIntegrationTest.SendRegionStillWaitsForAckBeforeReturning"
```

Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add src/kv.cpp
git commit -m "Defer KV sender ack unsubscribe cleanup"
```

## Task 3: Validate on VM1/VM2 and record sender effect

**Files:**
- No required code changes
- Optional docs update after validation

- [ ] **Step 1: VM1 focused validation**

Run:

```bash
cmake --build build -j4 --target test_message_kv test_message_kv_demo
./build/test_message_kv --gtest_filter="KvIntegrationTest.SendRegionAsyncCompletesAfterAckAndCleanup:KvIntegrationTest.AsyncSendCleanupCompletesByAckArrivalOrder:KvIntegrationTest.SendRegionAsyncFailsIfStoppedBeforeAck:KvIntegrationTest.SendRegionStillWaitsForAckBeforeReturning"
./build/test_message_kv_demo
```

- [ ] **Step 2: VM1/VM2 sender comparison**

Run `message_kv_demo` on TCP and RDMA with:

- `--send-mode sync`
- `--send-mode async`
- `--sizes 64K,1M`
- `--warmup-rounds 1`

Focus on:

- sender per-thread `send_us`
- sender `SEND_ROUND total_us`

Expected:

- modest improvement on sync and async sender completion
- no receiver regression

- [ ] **Step 3: Commit validation-only notes if docs are updated**

If you update the report, use a separate commit:

```bash
git add docs/reports/current-zerokv-implementation.md
git commit -m "Document KV lazy ack unsubscribe results"
```
