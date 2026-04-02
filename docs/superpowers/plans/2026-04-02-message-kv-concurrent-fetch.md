# MessageKV Concurrent Fetch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Improve `MessageKV::recv_batch()` large-payload throughput by allowing multiple ready keys to have `fetch_to()` operations in flight concurrently, while preserving the existing public API and ack semantics.

**Architecture:** Keep `MessageKV` single-threaded and state-machine-driven. Reuse `KVNode::wait_for_any_subscription_event(...)` to discover ready keys, then track `pending`, `ready`, and `in_flight` receive work explicitly inside `recv_batch()`. Ack remains per-key and is published only after that key's fetch completes successfully.

**Tech Stack:** C++20, ZeroKV `MessageKV`, `KVNode`, GoogleTest integration tests, VM1/VM2 TCP and RDMA validation.

---

### Task 1: Add Failing Coverage For Concurrent Receive Progress

**Files:**
- Modify: `tests/integration/test_message_kv.cpp`
- Test: `tests/integration/test_message_kv.cpp`

- [ ] **Step 1: Add a test that requires early per-key progress to coexist with another large pending key**

```cpp
TEST_F(MessageKvIntegrationTest, RecvBatchCanCompleteOneLargeKeyWhileAnotherIsStillInFlight) {
    auto sender = MessageKV::create(cfg_);
    auto receiver = MessageKV::create(cfg_);
    sender->start(sender_node_cfg_);
    receiver->start(receiver_node_cfg_);

    auto ctx = Context::create(cfg_);
    auto recv_region = MemoryRegion::allocate(ctx, 2 * 1024 * 1024);
    ASSERT_NE(recv_region, nullptr);

    std::string payload_a(1024 * 1024, 'a');
    std::string payload_b(1024 * 1024, 'b');

    std::thread delayed_sender([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        sender->send("large-b", payload_b.data(), payload_b.size());
    });

    std::thread immediate_sender([&] {
        sender->send("large-a", payload_a.data(), payload_a.size());
    });

    auto result = receiver->recv_batch({
        {.key = "large-a", .length = payload_a.size(), .offset = 0},
        {.key = "large-b", .length = payload_b.size(), .offset = payload_a.size()},
    }, recv_region, std::chrono::milliseconds(5000));

    immediate_sender.join();
    delayed_sender.join();

    EXPECT_TRUE(result.completed_all);
    EXPECT_EQ(result.failed.size(), 0u);
    EXPECT_EQ(result.timed_out.size(), 0u);
}
```

- [ ] **Step 2: Run the focused test and confirm current behavior before implementation**

Run:

```bash
cmake --build build -j4 --target test_message_kv
./build/test_message_kv --gtest_filter="MessageKvIntegrationTest.RecvBatchCanCompleteOneLargeKeyWhileAnotherIsStillInFlight"
```

Expected:

- either FAIL because the current serial receive path does not satisfy the new timing-sensitive assertions you add
- or PASS without proving overlap, in which case tighten the test to assert the intended sequencing before moving on

- [ ] **Step 3: Add a timeout-regression test that keeps partial-progress semantics intact**

```cpp
TEST_F(MessageKvIntegrationTest, RecvBatchConcurrentFetchStillReportsTimeoutsPrecisely) {
    auto sender = MessageKV::create(cfg_);
    auto receiver = MessageKV::create(cfg_);
    sender->start(sender_node_cfg_);
    receiver->start(receiver_node_cfg_);

    auto ctx = Context::create(cfg_);
    auto recv_region = MemoryRegion::allocate(ctx, 2 * 1024 * 1024);
    ASSERT_NE(recv_region, nullptr);

    std::string payload(1024 * 1024, 'x');
    sender->send("only-present", payload.data(), payload.size());

    auto result = receiver->recv_batch({
        {.key = "only-present", .length = payload.size(), .offset = 0},
        {.key = "missing", .length = payload.size(), .offset = payload.size()},
    }, recv_region, std::chrono::milliseconds(400));

    EXPECT_EQ(result.completed.size(), 1u);
    EXPECT_EQ(result.failed.size(), 0u);
    EXPECT_EQ(result.timed_out.size(), 1u);
    EXPECT_FALSE(result.completed_all);
}
```

- [ ] **Step 4: Run the timeout-focused test**

Run:

```bash
./build/test_message_kv --gtest_filter="MessageKvIntegrationTest.RecvBatchConcurrentFetchStillReportsTimeoutsPrecisely"
```

Expected:

- PASS on current behavior or after the next task
- if it fails after implementation, treat that as a correctness regression and fix before moving on

- [ ] **Step 5: Commit the test-only change**

```bash
git add tests/integration/test_message_kv.cpp
git commit -m "Add MessageKV concurrent recv batch tests"
```

### Task 2: Implement Concurrent In-Flight Fetches In `recv_batch()`

**Files:**
- Modify: `src/message_kv.cpp`
- Test: `tests/integration/test_message_kv.cpp`

- [ ] **Step 1: Replace the serial per-key receive loop with explicit per-key state**

Add internal state near `MessageKV::recv_batch()`:

```cpp
enum class ReceiveState {
    kPending,
    kReady,
    kInFlight,
    kCompleted,
    kFailed,
    kTimedOut,
};

struct InFlightFetch {
    std::string key;
    std::vector<const BatchRecvItem*> placements;
    std::vector<zerokv::Future<void>> futures;
};
```

Use maps/sets inside `recv_batch()`:

```cpp
std::unordered_map<std::string, ReceiveState> state_by_key;
std::unordered_map<std::string, InFlightFetch> in_flight;
std::unordered_set<std::string> pending;
std::unordered_set<std::string> ready;
constexpr size_t kMaxConcurrentFetches = 4;
```

- [ ] **Step 2: Split the current `try_fetch_key` helper into launch and completion phases**

Replace the existing serial helper with two helpers:

```cpp
auto try_fetch_key_immediately = [&](const std::string& key) -> PlacementState {
    const auto& placements = placements_by_key.at(key);
    for (const auto* placement : placements) {
        auto fetch = impl_->node->fetch_to(key, region, placement->length, placement->offset);
        if (!fetch.status().ok()) {
            if (fetch.status().code() == ErrorCode::kInvalidArgument) {
                return PlacementState::kPending;
            }
            return PlacementState::kFailed;
        }
        fetch.get();
        if (!fetch.status().ok()) {
            if (fetch.status().code() == ErrorCode::kInvalidArgument) {
                return PlacementState::kPending;
            }
            return PlacementState::kFailed;
        }
    }
    return PlacementState::kSuccess;
};

auto launch_fetch_for_key = [&](const std::string& key) -> bool {
    const auto& placements = placements_by_key.at(key);
    InFlightFetch launched{.key = key, .placements = placements};
    launched.futures.reserve(placements.size());
    for (const auto* placement : placements) {
        auto fetch = impl_->node->fetch_to(key, region, placement->length, placement->offset);
        if (!fetch.status().ok()) {
            if (fetch.status().code() == ErrorCode::kInvalidArgument) {
                return false;
            }
            state_by_key[key] = ReceiveState::kFailed;
            append_placements(&result.failed, placements);
            return true;
        }
        launched.futures.push_back(std::move(fetch));
    }
    in_flight.emplace(key, std::move(launched));
    state_by_key[key] = ReceiveState::kInFlight;
    return true;
};
```

- [ ] **Step 3: Add a completion pass that drains finished in-flight fetches**

Inside `recv_batch()`, add:

```cpp
auto poll_in_flight = [&]() {
    std::vector<std::string> finished_keys;
    for (auto& [key, fetches] : in_flight) {
        bool all_done = true;
        bool any_failed = false;
        for (auto& future : fetches.futures) {
            future.get();
            if (!future.status().ok()) {
                any_failed = true;
                break;
            }
        }
        if (!all_done) {
            continue;
        }
        finished_keys.push_back(key);
        if (any_failed) {
            state_by_key[key] = ReceiveState::kFailed;
            append_placements(&result.failed, fetches.placements);
            continue;
        }
        state_by_key[key] = ReceiveState::kCompleted;
        append_placements(&result.completed, fetches.placements);
        ack_completed_key(key);
        completed_at_us[key] = elapsed_us(recv_start, SteadyClock::now());
    }
    for (const auto& key : finished_keys) {
        in_flight.erase(key);
        pending.erase(key);
        ready.erase(key);
    }
};
```

Adjust the exact future handling to match the real `Future<void>` API in this codebase. The essential behavior is:

- launch fetches
- keep them tracked separately
- complete and ack them in a dedicated pass

- [ ] **Step 4: Rewrite the main loop to interleave event waiting, launch, and completion**

Use this structure inside `recv_batch()`:

```cpp
while ((!pending.empty() || !in_flight.empty()) && SteadyClock::now() < deadline) {
    while (!ready.empty() && in_flight.size() < kMaxConcurrentFetches) {
        auto it = ready.begin();
        const auto key = *it;
        ready.erase(it);
        launch_fetch_for_key(key);
    }

    poll_in_flight();
    if (pending.empty() && in_flight.empty()) {
        break;
    }

    std::vector<std::string> wait_keys;
    wait_keys.reserve(pending.size());
    for (const auto& key : pending) {
        if (state_by_key[key] == ReceiveState::kPending) {
            wait_keys.push_back(key);
        }
    }

    if (wait_keys.empty()) {
        continue;
    }

    const auto now = SteadyClock::now();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    auto event = impl_->node->wait_for_any_subscription_event(wait_keys, remaining);
    if (!event.has_value()) {
        break;
    }
    if (pending.count(event->key) && state_by_key[event->key] == ReceiveState::kPending) {
        state_by_key[event->key] = ReceiveState::kReady;
        ready.insert(event->key);
    }
}
```

After the loop:

- run a final `poll_in_flight()`
- classify any remaining pending/in-flight keys as `timed_out`
- keep unsubscribe and cleanup behavior intact

- [ ] **Step 5: Keep trace concise and state-oriented**

Update `ZEROKV_MESSAGE_KV_TRACE` lines to capture only state transitions:

```cpp
trace_message_kv("MESSAGE_KV_RECV_BATCH_FETCH_LAUNCH key=" + key +
                 " in_flight=" + std::to_string(in_flight.size()));
trace_message_kv("MESSAGE_KV_RECV_BATCH_FETCH_COMPLETE key=" + key +
                 " in_flight=" + std::to_string(in_flight.size()));
trace_message_kv("MESSAGE_KV_RECV_BATCH_WAIT_ANY_MATCH key=" + event->key);
```

Do not add noisy per-iteration traces when nothing changes.

- [ ] **Step 6: Run focused tests for the new receive path**

Run:

```bash
cmake --build build -j4 --target test_message_kv
./build/test_message_kv --gtest_filter="MessageKvIntegrationTest.RecvBatchCanCompleteOneLargeKeyWhileAnotherIsStillInFlight:MessageKvIntegrationTest.RecvBatchConcurrentFetchStillReportsTimeoutsPrecisely:MessageKvIntegrationTest.RecvBatchAcknowledgesCompletedKeyBeforeBatchFinishes:MessageKvIntegrationTest.RecvBatchReturnsPartialTimeout:MessageKvIntegrationTest.RecvCopiesSingleMessageIntoRegion"
```

Expected:

- all listed tests PASS

- [ ] **Step 7: Commit the implementation**

```bash
git add src/message_kv.cpp tests/integration/test_message_kv.cpp
git commit -m "Add concurrent fetch path to MessageKV recv_batch"
```

### Task 3: Validate Throughput And Preserve Existing Behavior

**Files:**
- Modify: `README.md` (only if measurement notes need updating)
- Test: `tests/integration/test_kv_node.cpp`
- Test: `tests/integration/test_message_kv.cpp`

- [ ] **Step 1: Keep the existing `wait_for_any_subscription_event(...)` coverage green**

Run:

```bash
cmake --build build -j4 --target test_kv_node
./build/test_kv_node --gtest_filter="KvNodeIntegrationTest.WaitForAnySubscriptionEventReturnsMatchingKey"
```

Expected:

- PASS

- [ ] **Step 2: Run the full local `MessageKV` integration suite relevant to receive correctness**

Run:

```bash
./build/test_message_kv
```

Expected:

- all MessageKV integration tests PASS

- [ ] **Step 3: Run dual-node TCP validation on VM1/VM2**

On VM1:

```bash
cd ~/zerokv-task
export UCX_NET_DEVICES=enp0s1
./build/message_kv_demo \
  --role rank0 \
  --listen 10.0.0.1:16000 \
  --data-addr 10.0.0.1:0 \
  --node-id rank0-receiver \
  --messages 4 \
  --sizes 1K,1M \
  --warmup-rounds 1 \
  --timeout-ms 180000 \
  --transport tcp
```

On VM2:

```bash
cd ~/zerokv-task
export UCX_NET_DEVICES=enp0s1
./build/message_kv_demo \
  --role rank1 \
  --server-addr 10.0.0.1:16000 \
  --data-addr 10.0.0.2:0 \
  --node-id rank1-sender \
  --threads 4 \
  --sizes 1K,1M \
  --warmup-rounds 1 \
  --timeout-ms 180000 \
  --transport tcp
```

Expected:

- both rounds PASS
- no sender `SEND_ERR`
- `RECV_ROUND completed=4 failed=0 timed_out=0 completed_all=1`

- [ ] **Step 4: Run dual-node RDMA validation on VM1/VM2**

On VM1:

```bash
cd ~/zerokv-task
export UCX_NET_DEVICES=rxe0:1
./build/message_kv_demo \
  --role rank0 \
  --listen 10.0.0.1:16000 \
  --data-addr 10.0.0.1:0 \
  --node-id rank0-receiver \
  --messages 4 \
  --sizes 1K,1M \
  --warmup-rounds 1 \
  --timeout-ms 180000 \
  --transport rdma
```

On VM2:

```bash
cd ~/zerokv-task
export UCX_NET_DEVICES=rxe0:1
./build/message_kv_demo \
  --role rank1 \
  --server-addr 10.0.0.1:16000 \
  --data-addr 10.0.0.2:0 \
  --node-id rank1-sender \
  --threads 4 \
  --sizes 1K,1M \
  --warmup-rounds 1 \
  --timeout-ms 180000 \
  --transport rdma
```

Expected:

- both rounds PASS
- no `remote access error`
- no `Connection reset`

- [ ] **Step 5: Run a large-payload spot check on VM1/VM2**

Use RDMA with:

```bash
--sizes 1M,16M,64M
```

Record:

- sender `SEND_ROUND`
- receiver `RECV_ROUND`

Expected:

- all three rounds PASS
- receiver throughput should improve relative to the current pre-change baseline

- [ ] **Step 6: Update README only if user-facing performance guidance changes**

If no CLI or semantics changed, skip README edits.

If a short note is needed, keep it minimal:

```md
- `recv_batch()` keeps the same public behavior but now allows more overlap in receive-side data movement for large payloads.
```

- [ ] **Step 7: Commit validation-only or doc follow-up if needed**

If no files changed in this task, do not create a no-op commit.

If README changed:

```bash
git add README.md
git commit -m "Document MessageKV concurrent fetch behavior"
```
