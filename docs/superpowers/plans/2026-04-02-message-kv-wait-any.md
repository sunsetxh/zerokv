# MessageKV Wait-Any Receive Path Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the current batch receive barrier with a single-threaded wait-any / per-key fetch / per-key ack path while keeping the public MessageKV API unchanged.

**Architecture:** Add a blocking `KVNode::wait_for_any_subscription_event(...)` helper that waits on the existing subscription queue and condition variable, then rewrite `MessageKV::recv_batch()` as a single-threaded state machine that subscribes all keys, reacts to whichever key becomes ready first, fetches that key into the caller region, and publishes its ack immediately. Keep trace logging behind `ZEROKV_KV_TRACE` and `ZEROKV_MESSAGE_KV_TRACE`.

**Tech Stack:** C++20, ZeroKV KVNode/MessageKV, GoogleTest integration tests, QEMU VM1/VM2 TCP + Soft-RoCE validation.

---

## File Map

- Modify: `include/zerokv/kv.h`
  - Add `wait_for_any_subscription_event(...)` declaration.
- Modify: `src/kv/node.cpp`
  - Implement `wait_for_any_subscription_event(...)`.
  - Keep subscription queue handling compatible with existing event wait helpers.
  - Retain concise `ZEROKV_KV_TRACE` hooks.
- Modify: `src/message_kv.cpp`
  - Add local batch layout validation for `MessageKV::recv_batch()`.
  - Rewrite `recv_batch()` into subscribe-all / wait-any / fetch-to / ack state machine.
  - Keep `ZEROKV_MESSAGE_KV_TRACE` hooks concise.
- Modify: `tests/integration/test_kv_node.cpp`
  - Add `wait_for_any_subscription_event(...)` integration coverage.
- Modify: `tests/integration/test_message_kv.cpp`
  - Add coverage proving a completed key is acknowledged before a slower sibling key finishes.
  - Preserve existing send/recv/timeout tests.

## Task 1: Add `KVNode::wait_for_any_subscription_event(...)`

**Files:**
- Modify: `include/zerokv/kv.h`
- Modify: `src/kv/node.cpp`
- Test: `tests/integration/test_kv_node.cpp`

- [ ] **Step 1: Write the failing KVNode integration test**

Add this test to `tests/integration/test_kv_node.cpp`:

```cpp
TEST_F(KvNodeIntegrationTest, WaitForAnySubscriptionEventReturnsMatchingKey) {
    auto server = KVServer::create(cfg_);
    ASSERT_NO_THROW(server->start({"127.0.0.1:0"}));

    auto publisher = KVNode::create(cfg_);
    auto subscriber = KVNode::create(cfg_);
    ASSERT_NO_THROW(publisher->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "publisher",
    }));
    ASSERT_NO_THROW(subscriber->start(NodeConfig{
        .server_addr = server->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "subscriber",
    }));

    subscriber->subscribe("key-a").status().throw_if_error();
    subscriber->subscribe("key-b").status().throw_if_error();

    const std::string payload = "hello";
    publisher->publish("key-b", payload.data(), payload.size()).status().throw_if_error();

    auto event = subscriber->wait_for_any_subscription_event(
        {"key-a", "key-b"}, std::chrono::milliseconds(1000));

    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->key, "key-b");
    EXPECT_EQ(event->type, SubscriptionEventType::kPublished);

    subscriber->stop();
    publisher->stop();
    server->stop();
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run on VM1:

```bash
cd ~/zerokv-task
cmake --build build -j4 --target test_kv_node
./build/test_kv_node --gtest_filter="KvNodeIntegrationTest.WaitForAnySubscriptionEventReturnsMatchingKey"
```

Expected: build or link failure because `wait_for_any_subscription_event(...)` is not yet declared/defined.

- [ ] **Step 3: Add the public declaration**

In `include/zerokv/kv.h`, add:

```cpp
std::optional<SubscriptionEvent> wait_for_any_subscription_event(
    const std::vector<std::string>& keys,
    std::chrono::milliseconds timeout);
```

Place it next to `wait_for_subscription_event(...)`.

- [ ] **Step 4: Implement the helper in `src/kv/node.cpp`**

Add logic equivalent to:

```cpp
std::optional<SubscriptionEvent> KVNode::wait_for_any_subscription_event(
    const std::vector<std::string>& keys,
    std::chrono::milliseconds timeout) {
    if (!impl_) {
        return std::nullopt;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->state_mu_);
        if (!impl_->running_) {
            Status(ErrorCode::kConnectionRefused,
                   "kv node is not running").throw_if_error();
        }
    }

    if (keys.empty()) {
        Status(ErrorCode::kInvalidArgument,
               "at least one key is required").throw_if_error();
    }

    std::unordered_set<std::string> wanted;
    wanted.reserve(keys.size());
    for (const auto& key : keys) {
        wanted.insert(key);
    }

    const auto deadline = SteadyClock::now() + timeout;
    std::unique_lock<std::mutex> lock(impl_->subscription_mu_);
    for (;;) {
        std::deque<SubscriptionEvent> unmatched;
        std::optional<SubscriptionEvent> matched;
        while (!impl_->subscription_events_.empty()) {
            auto event = std::move(impl_->subscription_events_.front());
            impl_->subscription_events_.pop_front();
            if ((event.type == SubscriptionEventType::kPublished ||
                 event.type == SubscriptionEventType::kUpdated) &&
                wanted.find(event.key) != wanted.end()) {
                matched = std::move(event);
                break;
            }
            unmatched.push_back(std::move(event));
        }
        while (!unmatched.empty()) {
            impl_->subscription_events_.push_front(std::move(unmatched.back()));
            unmatched.pop_back();
        }
        if (matched.has_value()) {
            return matched;
        }

        if (timeout <= std::chrono::milliseconds::zero()) {
            return std::nullopt;
        }
        if (!impl_->subscription_cv_.wait_until(lock, deadline, [&] {
                return !impl_->subscription_events_.empty();
            })) {
            return std::nullopt;
        }
    }
}
```

Keep the existing `ZEROKV_KV_TRACE` hook style if already present, but limit it to:
- start
- match
- timeout

- [ ] **Step 5: Run the test to verify it passes**

Run on VM1:

```bash
cd ~/zerokv-task
cmake --build build -j4 --target test_kv_node
./build/test_kv_node --gtest_filter="KvNodeIntegrationTest.WaitForAnySubscriptionEventReturnsMatchingKey"
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/zerokv/kv.h src/kv/node.cpp tests/integration/test_kv_node.cpp
git commit -m "Add KVNode wait-for-any subscription helper"
```

## Task 2: Rewrite `MessageKV::recv_batch()` as wait-any state machine

**Files:**
- Modify: `src/message_kv.cpp`
- Test: `tests/integration/test_message_kv.cpp`

- [ ] **Step 1: Write the failing MessageKV test**

Add this test to `tests/integration/test_message_kv.cpp`:

```cpp
TEST_F(MessageKvIntegrationTest,
       RecvBatchAcknowledgesCompletedKeyBeforeBatchFinishes) {
    auto sender_a = MessageKV::create(cfg_);
    auto sender_b = MessageKV::create(cfg_);
    auto receiver = MessageKV::create(cfg_);

    ASSERT_NO_THROW(receiver->start(NodeConfig{
        .server_addr = server_->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "receiver",
    }));
    ASSERT_NO_THROW(sender_a->start(NodeConfig{
        .server_addr = server_->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "sender-a",
    }));
    ASSERT_NO_THROW(sender_b->start(NodeConfig{
        .server_addr = server_->address(),
        .local_data_addr = "127.0.0.1:0",
        .node_id = "sender-b",
    }));

    auto ctx = Context::create(cfg_);
    auto region = ctx->allocate(32);
    ASSERT_TRUE(region);

    std::future<MessageKV::BatchRecvResult> recv_future =
        std::async(std::launch::async, [&] {
            return receiver->recv_batch({
                MessageKV::BatchRecvItem{.key = "batch-fast", .length = 16, .offset = 0},
                MessageKV::BatchRecvItem{.key = "batch-slow", .length = 16, .offset = 16},
            }, region, std::chrono::milliseconds(1000));
        });

    std::future<void> send_fast = std::async(std::launch::async, [&] {
        sender_a->send("batch-fast", "fast", 4);
    });

    EXPECT_EQ(send_fast.wait_for(std::chrono::milliseconds(300)),
              std::future_status::ready);

    sender_b->send("batch-slow", "slow", 4);

    auto result = recv_future.get();
    EXPECT_TRUE(result.completed_all);
    EXPECT_TRUE(result.failed.empty());
    EXPECT_TRUE(result.timed_out.empty());

    sender_a->stop();
    sender_b->stop();
    receiver->stop();
}
```

- [ ] **Step 2: Run the test to verify it fails against the old batch barrier**

Run on VM1:

```bash
cd ~/zerokv-task
cmake --build build -j4 --target test_message_kv
./build/test_message_kv --gtest_filter="MessageKvIntegrationTest.RecvBatchAcknowledgesCompletedKeyBeforeBatchFinishes"
```

Expected: FAIL because the fast sender does not finish before the slow sibling key arrives.

- [ ] **Step 3: Add local batch layout validation**

In `src/message_kv.cpp`, add a helper:

```cpp
void validate_recv_batch_layout(
    const std::vector<MessageKV::BatchRecvItem>& items,
    const MemoryRegion::Ptr& region) {
    if (!region) {
        Status(ErrorCode::kInvalidArgument,
               "local region is required").throw_if_error();
    }
    if (items.empty()) {
        Status(ErrorCode::kInvalidArgument,
               "at least one batch item is required").throw_if_error();
    }

    struct Range {
        size_t begin;
        size_t end;
    };
    std::vector<Range> ranges;
    ranges.reserve(items.size());

    for (const auto& item : items) {
        if (item.key.empty()) {
            Status(ErrorCode::kInvalidArgument,
                   "batch item key is empty").throw_if_error();
        }
        if (item.length == 0) {
            Status(ErrorCode::kInvalidArgument,
                   "batch item length must be > 0").throw_if_error();
        }
        if (item.offset > region->length() ||
            item.length > region->length() - item.offset) {
            Status(ErrorCode::kInvalidArgument,
                   "batch item range exceeds local region").throw_if_error();
        }
        ranges.push_back({item.offset, item.offset + item.length});
    }

    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) {
        return a.begin < b.begin || (a.begin == b.begin && a.end < b.end);
    });
    for (size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i - 1].end > ranges[i].begin) {
            Status(ErrorCode::kInvalidArgument,
                   "batch item output ranges overlap").throw_if_error();
        }
    }
}
```

- [ ] **Step 4: Rewrite `recv_batch()` into a wait-any loop**

Replace the current delegation with logic shaped like:

```cpp
MessageKV::BatchRecvResult MessageKV::recv_batch(
    const std::vector<BatchRecvItem>& items,
    const MemoryRegion::Ptr& region,
    std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    validate_recv_batch_layout(items, region);
    impl_->node_ready_locked();
    impl_->sweep_cleanup_locked();

    std::unordered_map<std::string, std::vector<const BatchRecvItem*>> placements_by_key;
    std::vector<std::string> ordered_keys;
    for (const auto& item : items) {
        auto& placements = placements_by_key[item.key];
        if (placements.empty()) {
            ordered_keys.push_back(item.key);
        }
        placements.push_back(&item);
    }

    for (const auto& key : ordered_keys) {
        impl_->node->subscribe(key).status().throw_if_error();
    }

    auto append_completed = [&](const std::string& key, BatchRecvResult* result) {
        const auto& placements = placements_by_key.at(key);
        for (const auto* placement : placements) {
            result->completed.push_back(placement->key);
        }
    };

    auto append_failed = [&](const std::string& key, BatchRecvResult* result) {
        const auto& placements = placements_by_key.at(key);
        for (const auto* placement : placements) {
            result->failed.push_back(placement->key);
        }
    };

    auto try_fetch_key = [&](const std::string& key) -> int {
        const auto& placements = placements_by_key.at(key);
        for (const auto* placement : placements) {
            auto fetch = impl_->node->fetch_to(
                key, region, placement->length, placement->offset);
            if (!fetch.status().ok()) {
                if (fetch.status().code() == ErrorCode::kInvalidArgument) {
                    return 0;  // still pending / not yet visible
                }
                return -1;     // terminal failure
            }
            fetch.get();
        }
        return 1;  // success
    };

    BatchRecvResult result;
    std::unordered_set<std::string> pending(ordered_keys.begin(), ordered_keys.end());
    const auto deadline = SteadyClock::now() + timeout;

    for (const auto& key : ordered_keys) {
        const int state = try_fetch_key(key);
        if (state == 1) {
            append_completed(key, &result);
            impl_->publish_ack_locked(key);
            impl_->record_owned_ack_key_locked(key);
            pending.erase(key);
        } else if (state < 0) {
            append_failed(key, &result);
            pending.erase(key);
        }
    }

    while (!pending.empty()) {
        const auto now = SteadyClock::now();
        if (now >= deadline) {
            break;
        }

        std::vector<std::string> pending_keys(pending.begin(), pending.end());
        auto event = impl_->node->wait_for_any_subscription_event(
            pending_keys,
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        if (!event.has_value()) {
            break;
        }

        const int state = try_fetch_key(event->key);
        if (state == 1) {
            append_completed(event->key, &result);
            impl_->publish_ack_locked(event->key);
            impl_->record_owned_ack_key_locked(event->key);
            pending.erase(event->key);
        } else if (state < 0) {
            append_failed(event->key, &result);
            pending.erase(event->key);
        }
    }

    for (const auto& key : ordered_keys) {
        impl_->node->unsubscribe(key).status().throw_if_error();
    }
    for (const auto& key : ordered_keys) {
        if (pending.find(key) != pending.end()) {
            result.timed_out.push_back(key);
        }
    }
    result.completed_all = (result.completed.size() == items.size());
    return result;
}
```

Preserve existing `recv()` wrapper behavior.

- [ ] **Step 5: Keep trace hooks concise**

In `src/message_kv.cpp`, if `ZEROKV_MESSAGE_KV_TRACE` support already exists, keep only:
- batch start
- immediate fetch success/failure
- wait-any match
- per-key fetch begin/done
- per-key ack publish
- final batch summary

Do not add uncontrolled per-iteration sleep/poll trace.

- [ ] **Step 6: Run the targeted MessageKV tests**

Run on VM1:

```bash
cd ~/zerokv-task
cmake --build build -j4 --target test_message_kv
./build/test_message_kv --gtest_filter="MessageKvIntegrationTest.RecvBatchAcknowledgesCompletedKeyBeforeBatchFinishes:MessageKvIntegrationTest.RecvBatchReturnsPartialTimeout:MessageKvIntegrationTest.RecvCopiesSingleMessageIntoRegion:MessageKvIntegrationTest.SendUnpublishesMessageKeyAfterAck"
```

Expected: all PASS.

- [ ] **Step 7: Commit**

```bash
git add src/message_kv.cpp tests/integration/test_message_kv.cpp
git commit -m "Rewrite MessageKV recv_batch with wait-any flow"
```

## Task 3: Validate performance direction on VM1/VM2

**Files:**
- Modify: none required unless a minimal trace cleanup is needed
- Test via runtime commands on VM1/VM2

- [ ] **Step 1: Sync changed files to VM1/VM2**

Run locally:

```bash
scp -P 2222 -o StrictHostKeyChecking=no include/zerokv/kv.h axon@localhost:~/zerokv-task/include/zerokv/kv.h
scp -P 2222 -o StrictHostKeyChecking=no src/kv/node.cpp axon@localhost:~/zerokv-task/src/kv/node.cpp
scp -P 2222 -o StrictHostKeyChecking=no src/message_kv.cpp axon@localhost:~/zerokv-task/src/message_kv.cpp
scp -P 2222 -o StrictHostKeyChecking=no tests/integration/test_kv_node.cpp axon@localhost:~/zerokv-task/tests/integration/test_kv_node.cpp
scp -P 2222 -o StrictHostKeyChecking=no tests/integration/test_message_kv.cpp axon@localhost:~/zerokv-task/tests/integration/test_message_kv.cpp

scp -P 2223 -o StrictHostKeyChecking=no include/zerokv/kv.h axon@localhost:~/zerokv-task/include/zerokv/kv.h
scp -P 2223 -o StrictHostKeyChecking=no src/kv/node.cpp axon@localhost:~/zerokv-task/src/kv/node.cpp
scp -P 2223 -o StrictHostKeyChecking=no src/message_kv.cpp axon@localhost:~/zerokv-task/src/message_kv.cpp
```

- [ ] **Step 2: Rebuild on VM1 and VM2**

Run:

```bash
sshpass -p axon ssh -p 2222 -o StrictHostKeyChecking=no axon@localhost \
  'cd ~/zerokv-task && cmake --build build -j4 --target message_kv_demo test_message_kv test_kv_node'

sshpass -p axon ssh -p 2223 -o StrictHostKeyChecking=no axon@localhost \
  'cd ~/zerokv-task && cmake --build build -j4 --target message_kv_demo'
```

Expected: successful build on both VMs.

- [ ] **Step 3: Run dual-node TCP validation**

On VM1:

```bash
cd ~/zerokv-task
env ZEROKV_MESSAGE_KV_TRACE=1 ZEROKV_KV_TRACE=1 UCX_NET_DEVICES=enp0s1 \
  ./build/message_kv_demo \
    --role rank0 \
    --listen 10.0.0.1:16610 \
    --data-addr 10.0.0.1:0 \
    --node-id rank0-receiver \
    --messages 4 \
    --sizes 1K \
    --warmup-rounds 1 \
    --timeout-ms 30000 \
    --post-recv-wait-ms 1000 \
    --transport tcp
```

On VM2:

```bash
cd ~/zerokv-task
env ZEROKV_MESSAGE_KV_TRACE=1 ZEROKV_KV_TRACE=1 UCX_NET_DEVICES=enp0s1 \
  ./build/message_kv_demo \
    --role rank1 \
    --server-addr 10.0.0.1:16610 \
    --data-addr 10.0.0.2:0 \
    --node-id rank1-sender \
    --threads 4 \
    --sizes 1K \
    --warmup-rounds 1 \
    --timeout-ms 30000 \
    --transport tcp
```

Expected:
- measured `SEND_ROUND` stays in the low-millisecond range
- trace shows per-key ack arrival without a single final batch barrier

- [ ] **Step 4: Run dual-node RDMA validation**

On VM1:

```bash
cd ~/zerokv-task
env ZEROKV_MESSAGE_KV_TRACE=1 ZEROKV_KV_TRACE=1 UCX_NET_DEVICES=rxe0:1 \
  ./build/message_kv_demo \
    --role rank0 \
    --listen 10.0.0.1:16611 \
    --data-addr 10.0.0.1:0 \
    --node-id rank0-receiver \
    --messages 4 \
    --sizes 1K \
    --warmup-rounds 1 \
    --timeout-ms 30000 \
    --post-recv-wait-ms 1000 \
    --transport rdma
```

On VM2:

```bash
cd ~/zerokv-task
env ZEROKV_MESSAGE_KV_TRACE=1 ZEROKV_KV_TRACE=1 UCX_NET_DEVICES=rxe0:1 \
  ./build/message_kv_demo \
    --role rank1 \
    --server-addr 10.0.0.1:16611 \
    --data-addr 10.0.0.2:0 \
    --node-id rank1-sender \
    --threads 4 \
    --sizes 1K \
    --warmup-rounds 1 \
    --timeout-ms 30000 \
    --transport rdma
```

Expected:
- measured `SEND_ROUND` remains in the low-millisecond range after warmup
- sender thread `send_us` values should not regress back toward the old tens-of-ms barrier pattern

- [ ] **Step 5: Commit any trace cleanup if needed**

If the implementation required final trace cleanup adjustments during validation:

```bash
git add include/zerokv/kv.h src/kv/node.cpp src/message_kv.cpp tests/integration/test_kv_node.cpp tests/integration/test_message_kv.cpp
git commit -m "Polish MessageKV wait-any tracing"
```

If no cleanup was needed, skip this commit.

## Self-Review

- Spec coverage:
  - `wait_for_any_subscription_event(...)`: Task 1
  - `recv_batch()` wait-any rewrite: Task 2
  - trace retention: Tasks 1-2
  - VM validation: Task 3
- Placeholder scan:
  - no `TODO`/`TBD` placeholders remain
- Type consistency:
  - `wait_for_any_subscription_event(...)` uses `std::optional<SubscriptionEvent>`
  - `BatchRecvItem` / `BatchRecvResult` names match existing code
