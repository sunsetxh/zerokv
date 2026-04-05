# KV Async Send Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `KV::send_async()` and `KV::send_region_async()` so sender threads can publish large payloads without blocking on ack wait and metadata cleanup, while preserving existing synchronous `send()` / `send_region()` semantics.

**Architecture:** Keep the receiver path unchanged. Extend `KV::Impl` with a pending-send table, one cleanup thread, one condition variable, and a completion state object that can back a `transport::Future<void>`. Async sends publish immediately, enqueue cleanup work, and return a future; the cleanup thread waits for ack, unsubscribes the ack key, unpublishes the message key, and completes the future. The existing sync APIs become thin wrappers over the async path plus blocking wait.

**Tech Stack:** C++20, current `zerokv::transport::Future<void>` / `Request`, `zerokv::core::KVNode`, GoogleTest integration tests, VM1/VM2 TCP/RDMA validation.

---

## File Map

- Modify: `include/zerokv/kv.h`
  - add async sender method declarations and any helper types that must be public
- Modify: `src/kv.cpp`
  - add pending-send state, cleanup thread, completion mechanism, async sender implementation, and sync-wrapper refactor
- Modify: `tests/integration/test_message_kv.cpp`
  - add async sender API and lifecycle tests
- Modify: `examples/message_kv_demo.cpp`
  - optionally add an async sender mode for throughput measurement after correctness lands
- Modify: `README.md`
  - document the new async sender APIs after implementation is stable

## Task 1: Define and test the async sender API surface

**Files:**
- Modify: `include/zerokv/kv.h`
- Test: `tests/integration/test_message_kv.cpp`

- [ ] **Step 1: Write the failing API-surface tests**

Add tests that require the new methods to exist and return `zerokv::transport::Future<void>`.

```cpp
TEST(KvApiSurfaceTest, AsyncSenderMethodsExist) {
    using FutureType = zerokv::transport::Future<void>;
    static_assert(std::is_same_v<decltype(std::declval<zerokv::KV&>().send_async(
                      std::declval<const std::string&>(),
                      std::declval<const void*>(),
                      std::declval<size_t>())),
                  FutureType>);
    static_assert(std::is_same_v<decltype(std::declval<zerokv::KV&>().send_region_async(
                      std::declval<const std::string&>(),
                      std::declval<const zerokv::transport::MemoryRegion::Ptr&>(),
                      std::declval<size_t>())),
                  FutureType>);
}
```

- [ ] **Step 2: Run the focused test and verify it fails**

Run:
```bash
./build/test_message_kv --gtest_filter="KvApiSurfaceTest.AsyncSenderMethodsExist"
```

Expected: FAIL to compile because `send_async` / `send_region_async` do not exist yet.

- [ ] **Step 3: Add minimal public declarations**

In `include/zerokv/kv.h`, add:

```cpp
zerokv::transport::Future<void> send_async(const std::string& key,
                                           const void* data,
                                           size_t size);

zerokv::transport::Future<void> send_region_async(
    const std::string& key,
    const zerokv::transport::MemoryRegion::Ptr& region,
    size_t size);
```

Do not implement behavior yet beyond what is required to compile the tests.

- [ ] **Step 4: Rebuild and verify the API-surface test passes**

Run:
```bash
cmake --build build -j4 --target test_message_kv
./build/test_message_kv --gtest_filter="KvApiSurfaceTest.AsyncSenderMethodsExist"
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/zerokv/kv.h tests/integration/test_message_kv.cpp
git commit -m "Add KV async sender API surface"
```

## Task 2: Build the completion state and failing async lifecycle tests

**Files:**
- Modify: `src/kv.cpp`
- Test: `tests/integration/test_message_kv.cpp`

- [ ] **Step 1: Write failing lifecycle tests for async success and shutdown failure**

Add two tests.

```cpp
TEST_F(KvIntegrationTest, SendRegionAsyncCompletesAfterAckAndCleanup) {
    auto sender = zerokv::KV::create(cfg_);
    auto receiver = zerokv::KV::create(cfg_);
    sender->start(sender_cfg_);
    receiver->start(receiver_cfg_);

    auto send_region = sender->allocate_send_region(payload.size());
    std::memcpy(send_region->address(), payload.data(), payload.size());

    auto recv_region = zerokv::transport::MemoryRegion::allocate(ctx_, payload.size());
    auto future = sender->send_region_async("async-key", send_region, payload.size());
    receiver->recv("async-key", recv_region, payload.size(), 0, std::chrono::seconds(5));

    future.get();
    EXPECT_TRUE(future.status().ok());
    EXPECT_FALSE(sender_node_has_key("async-key"));
}

TEST_F(KvIntegrationTest, SendRegionAsyncFailsIfStoppedBeforeAck) {
    auto sender = zerokv::KV::create(cfg_);
    sender->start(sender_cfg_);

    auto send_region = sender->allocate_send_region(payload.size());
    std::memcpy(send_region->address(), payload.data(), payload.size());

    auto future = sender->send_region_async("async-stop", send_region, payload.size());
    sender->stop();

    future.get();
    EXPECT_EQ(future.status().code(), zerokv::ErrorCode::kConnectionReset);
}
```

- [ ] **Step 2: Run the focused tests and verify they fail**

Run:
```bash
./build/test_message_kv --gtest_filter="KvIntegrationTest.SendRegionAsyncCompletesAfterAckAndCleanup:KvIntegrationTest.SendRegionAsyncFailsIfStoppedBeforeAck"
```

Expected: FAIL or compile failure because the async implementation does not exist yet.

- [ ] **Step 3: Add private completion state to `KV::Impl`**

In `src/kv.cpp`, add an internal shared state object for each async send, for example:

```cpp
struct AsyncSendState {
    std::mutex mu;
    std::condition_variable cv;
    bool ready = false;
    zerokv::Status status = zerokv::Status::OK();
};
```

Add pending item storage in `KV::Impl`:

```cpp
struct PendingSend {
    std::string message_key;
    std::string ack_key;
    std::shared_ptr<AsyncSendState> state;
    bool subscribed = false;
};

std::condition_variable cleanup_cv;
std::thread cleanup_thread;
bool cleanup_stop = false;
std::deque<PendingSend> pending_sends;
```

- [ ] **Step 4: Add a minimal `Future<void>` bridge for the shared state**

Implement the smallest bridge that lets async code complete a `transport::Future<void>` from the cleanup thread. If `Promise<void>` is missing, add an internal helper in `src/kv.cpp` that wraps the shared state and exposes:

```cpp
zerokv::transport::Future<void> make_async_future(const std::shared_ptr<AsyncSendState>& state);
void fulfill_async_future(const std::shared_ptr<AsyncSendState>& state, zerokv::Status status);
```

The future must support:
- `get()` blocking until `ready`
- `status()` returning final status after completion

Keep this bridge private to `src/kv.cpp` for this phase.

- [ ] **Step 5: Run the focused tests again and confirm they still fail for missing cleanup logic**

Run the same gtest filter.

Expected: tests compile further but still fail because ack wait / unpublish lifecycle is not implemented yet.

- [ ] **Step 6: Commit**

```bash
git add src/kv.cpp tests/integration/test_message_kv.cpp
git commit -m "Add KV async send completion state"
```

## Task 3: Implement the cleanup thread and async sender flow

**Files:**
- Modify: `src/kv.cpp`
- Test: `tests/integration/test_message_kv.cpp`

- [ ] **Step 1: Start and stop the cleanup thread with the `KV` lifecycle**

In `KV::start()` after node startup, launch one cleanup thread:

```cpp
impl_->cleanup_stop = false;
impl_->cleanup_thread = std::thread([impl = impl_.get()] { impl->cleanup_loop(); });
```

In `KV::stop()`:

```cpp
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->cleanup_stop = true;
}
impl_->cleanup_cv.notify_all();
if (impl_->cleanup_thread.joinable()) {
    impl_->cleanup_thread.join();
}
```

Before join completes, mark any still-pending futures failed with `kConnectionReset` or `kTimeout`.

- [ ] **Step 2: Implement `send_async()` and `send_region_async()`**

Minimal flow:

```cpp
zerokv::transport::Future<void> KV::send_region_async(...) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->sweep_cleanup_locked();
    validate...;

    auto publish = impl_->node->publish_region(key, region, size);
    publish.get();
    publish.status().throw_if_error();

    auto state = std::make_shared<AsyncSendState>();
    impl_->pending_sends.push_back(PendingSend{
        .message_key = key,
        .ack_key = make_ack_key(key),
        .state = state,
        .subscribed = false,
    });
    impl_->cleanup_cv.notify_one();
    return make_async_future(state);
}
```

`send_async()` mirrors this with `publish()`.

- [ ] **Step 3: Implement the cleanup loop**

Add `KV::Impl::cleanup_loop()` that:

1. waits for pending work or stop
2. subscribes to `ack_key` if not already subscribed
3. drops the main mutex while waiting on ack
4. waits for the ack event
5. unsubscribes the ack key
6. unpublishes the message key
7. fulfills the future
8. removes the pending item

Pseudo-shape:

```cpp
void KV::Impl::cleanup_loop() {
    while (true) {
        PendingSend current;
        {
            std::unique_lock<std::mutex> lock(mu);
            cleanup_cv.wait(lock, [&] { return cleanup_stop || !pending_sends.empty(); });
            if (cleanup_stop && pending_sends.empty()) {
                return;
            }
            current = pending_sends.front();
        }

        Status st = subscribe_ack_wait_unpublish(current);
        fulfill_async_future(current.state, st);

        std::lock_guard<std::mutex> lock(mu);
        pop matching pending send from queue;
    }
}
```

- [ ] **Step 4: Run the async lifecycle tests and make them pass**

Run:
```bash
cmake --build build -j4 --target test_message_kv
./build/test_message_kv --gtest_filter="KvIntegrationTest.SendRegionAsyncCompletesAfterAckAndCleanup:KvIntegrationTest.SendRegionAsyncFailsIfStoppedBeforeAck"
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/kv.cpp tests/integration/test_message_kv.cpp
git commit -m "Implement KV async send cleanup loop"
```

## Task 4: Reuse the async path for existing synchronous send APIs

**Files:**
- Modify: `src/kv.cpp`
- Test: `tests/integration/test_message_kv.cpp`

- [ ] **Step 1: Add a failing regression test that existing sync sender behavior is unchanged**

If missing, add or tighten a test like:

```cpp
TEST_F(KvIntegrationTest, SendRegionStillWaitsForAckBeforeReturning) {
    auto sender = zerokv::KV::create(cfg_);
    auto receiver = zerokv::KV::create(cfg_);
    sender->start(sender_cfg_);
    receiver->start(receiver_cfg_);

    std::atomic<bool> send_returned{false};
    std::thread t([&] {
        sender->send_region("sync-key", send_region, payload.size());
        send_returned.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(send_returned.load());
    receiver->recv("sync-key", recv_region, payload.size(), 0, std::chrono::seconds(5));
    t.join();
    EXPECT_TRUE(send_returned.load());
}
```

- [ ] **Step 2: Refactor sync senders to delegate to async + wait**

In `src/kv.cpp`:

```cpp
void KV::send_region(...) {
    auto future = send_region_async(key, region, size);
    future.get();
    future.status().throw_if_error();
}
```

Do the same for `send()`.

- [ ] **Step 3: Run the old sender regression tests plus the new one**

Run:
```bash
./build/test_message_kv --gtest_filter="KvIntegrationTest.SendPublishesMessageKey:KvIntegrationTest.SendRegionPublishesMessageKey:KvIntegrationTest.SendRequiresRunningNodeAndValidatesInputs:KvIntegrationTest.SendRegionRequiresRunningNodeAndValidatesInputs:KvIntegrationTest.SendUnpublishesMessageKeyAfterAck:KvIntegrationTest.SendRegionStillWaitsForAckBeforeReturning"
```

Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add src/kv.cpp tests/integration/test_message_kv.cpp
git commit -m "Refactor KV sync senders over async path"
```

## Task 5: Add sender-oriented demo coverage and documentation

**Files:**
- Modify: `examples/message_kv_demo.cpp`
- Modify: `README.md`
- Test: `tests/integration/test_message_kv_demo.cpp`

- [ ] **Step 1: Add a minimal demo helper or flag test first**

If you add an async demo mode or helper, add a focused test for any new parsing/helper function before changing demo behavior.

Example:

```cpp
TEST(MessageKvDemoHelpersTest, AsyncModeDefaultsOff) {
    // Add only if a new helper is introduced.
}
```

If no helper is needed, skip this step and keep the demo unchanged in this task.

- [ ] **Step 2: Add optional async sender mode to `message_kv_demo`**

Recommended minimal flag:

```text
--send-mode sync|async
```

Default: `sync`

Async sender path in `run_rank1()`:

1. call `send_region_async()` for each thread
2. collect futures
3. wait for them after launch to keep round boundaries clean
4. still print `SEND_OK` and `SEND_ROUND`

This preserves the existing demo structure while allowing direct comparison.

- [ ] **Step 3: Update README with the new sender mode**

Document:
- sync mode keeps the current end-to-end semantics
- async mode measures sender throughput without blocking the application thread on ack wait
- completion still means ack + metadata cleanup finished

- [ ] **Step 4: Verify demo build and helper tests**

Run:
```bash
cmake --build build -j4 --target test_message_kv_demo message_kv_demo
./build/test_message_kv_demo
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add examples/message_kv_demo.cpp README.md tests/integration/test_message_kv_demo.cpp
git commit -m "Add KV async sender demo mode"
```

## Task 6: VM validation for async sender throughput

**Files:**
- No code changes required unless validation finds regressions

- [ ] **Step 1: Rebuild on VM1 and VM2**

Run on both VMs in `/tmp/zerokv-<commit>`:
```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DZEROKV_BUILD_TESTS=ON \
  -DZEROKV_BUILD_EXAMPLES=ON \
  -DZEROKV_BUILD_BENCHMARK=ON \
  -DZEROKV_BUILD_PYTHON=OFF
cmake --build build -j4 --target test_message_kv test_message_kv_demo message_kv_demo
```

- [ ] **Step 2: Run focused test suites on VM1**

Run:
```bash
./build/test_message_kv --gtest_filter="KvApiSurfaceTest.AsyncSenderMethodsExist:KvIntegrationTest.SendRegionAsyncCompletesAfterAckAndCleanup:KvIntegrationTest.SendRegionAsyncFailsIfStoppedBeforeAck:KvIntegrationTest.SendRegionStillWaitsForAckBeforeReturning"
./build/test_message_kv_demo
```

Expected: PASS.

- [ ] **Step 3: Run dual-node TCP comparison**

RANK0:
```bash
env LD_LIBRARY_PATH=/usr/local/lib UCX_NET_DEVICES=enp0s1 \
  ./build/message_kv_demo --role rank0 --listen 10.0.0.1:16040 --data-addr 10.0.0.1:0 \
  --node-id rank0-receiver --messages 4 --sizes 64K,1M --warmup-rounds 1 --timeout-ms 30000 --transport tcp
```

RANK1 sync:
```bash
env LD_LIBRARY_PATH=/usr/local/lib UCX_NET_DEVICES=enp0s1 \
  ./build/message_kv_demo --role rank1 --server-addr 10.0.0.1:16040 --data-addr 10.0.0.2:0 \
  --node-id rank1-sender --threads 4 --sizes 64K,1M --warmup-rounds 1 --timeout-ms 30000 --transport tcp --send-mode sync
```

RANK1 async:
```bash
env LD_LIBRARY_PATH=/usr/local/lib UCX_NET_DEVICES=enp0s1 \
  ./build/message_kv_demo --role rank1 --server-addr 10.0.0.1:16040 --data-addr 10.0.0.2:0 \
  --node-id rank1-sender --threads 4 --sizes 64K,1M --warmup-rounds 1 --timeout-ms 30000 --transport tcp --send-mode async
```

Expected: async sender `SEND_ROUND` is measurably better than sync for `1MiB`.

- [ ] **Step 4: Run dual-node RDMA comparison**

Repeat the same comparison with:
```bash
UCX_NET_DEVICES=rxe0:1
--transport rdma
```

Expected: async sender `SEND_ROUND` improves further on `1MiB+`, while receiver output stays functionally unchanged.

- [ ] **Step 5: Commit validation-only findings if code changed during validation**

If validation required code fixes:
```bash
git add <fixed files>
git commit -m "Fix KV async sender validation findings"
```

If not, no commit is needed.
