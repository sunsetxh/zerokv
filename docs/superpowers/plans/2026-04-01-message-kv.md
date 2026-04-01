# MessageKV Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a first `zerokv::MessageKV` wrapper that exposes message-style send/receive APIs on top of existing ZeroKV KV primitives, with internal ack-based cleanup hidden from callers.

**Architecture:** Phase 1 is a wrapper-only implementation. Sender-side operations publish message keys and record them as pending cleanup. Receiver-side operations consume via existing zero-copy wait-and-fetch helpers, then publish internal ack keys. Cleanup stays on `MessageKV`'s synchronous API path plus bounded `stop()` cleanup; no background KV thread is introduced. Business code provides unique keys and never sees ack keys.

**Tech Stack:** C++20, ZeroKV KVNode/KVNode helpers, GoogleTest integration tests, CMake

---

## File Map

**New public API**
- Create: `include/zerokv/message_kv.h` — `MessageKV`, `BatchRecvItem`, `BatchRecvResult`

**Implementation**
- Create: `src/message_kv.cpp` — wrapper state, ack key convention, cleanup sweep, send/recv methods

**Build wiring**
- Modify: `CMakeLists.txt` — add new source to library and install the new public header
- Modify: `include/zerokv/zerokv.h` — include `zerokv/message_kv.h`

**Tests**
- Modify: `tests/integration/test_kv_node.cpp` or create `tests/integration/test_message_kv.cpp` — end-to-end wrapper behavior

**Docs**
- Modify: `README.md` — mention `MessageKV` first scenario wrapper
- Modify: `docs/reports/zerokv-rdma-kv-mvp.md` — add `message_kv` status note if needed

### Task 1: Add Public MessageKV API Surface

**Files:**
- Create: `include/zerokv/message_kv.h`
- Modify: `include/zerokv/zerokv.h`
- Modify: `CMakeLists.txt`
- Test: `tests/integration/test_message_kv.cpp`

- [ ] **Step 1: Write the failing API-surface test**

Create `tests/integration/test_message_kv.cpp` with a compile-and-link smoke that instantiates the public types:

```cpp
#include "zerokv/message_kv.h"

#include <gtest/gtest.h>

TEST(MessageKvApiSurfaceTest, PublicTypesExist) {
    using zerokv::MessageKV;

    MessageKV::BatchRecvItem item;
    item.key = "k";
    item.length = 16;
    item.offset = 0;

    MessageKV::BatchRecvResult result;
    EXPECT_FALSE(result.completed_all);
}
```

- [ ] **Step 2: Run the new test target to confirm it fails**

Run:

```bash
cmake --build build -j4 --target test_message_kv
```

Expected:
- build fails because `zerokv/message_kv.h` does not exist yet

- [ ] **Step 3: Add the public header and umbrella include**

Create `include/zerokv/message_kv.h` with the minimal API:

```cpp
#pragma once

#include "zerokv/kv.h"
#include "zerokv/memory.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace zerokv {

class MessageKV {
public:
    using Ptr = std::shared_ptr<MessageKV>;

    struct BatchRecvItem {
        std::string key;
        size_t length = 0;
        size_t offset = 0;
    };

    struct BatchRecvResult {
        std::vector<std::string> completed;
        std::vector<std::string> failed;
        std::vector<std::string> timed_out;
        bool completed_all = false;
    };

    static Ptr create(const zerokv::Config& cfg = {});

    ~MessageKV();
    MessageKV(const MessageKV&) = delete;
    MessageKV& operator=(const MessageKV&) = delete;

    void start(const zerokv::kv::NodeConfig& cfg);
    void stop();

    void send(const std::string& key, const void* data, size_t size);
    void send_region(const std::string& key,
                     const zerokv::MemoryRegion::Ptr& region,
                     size_t size);

    void recv(const std::string& key,
              const zerokv::MemoryRegion::Ptr& region,
              size_t length,
              size_t offset,
              std::chrono::milliseconds timeout);

    BatchRecvResult recv_batch(const std::vector<BatchRecvItem>& items,
                               const zerokv::MemoryRegion::Ptr& region,
                               std::chrono::milliseconds timeout);

private:
    explicit MessageKV(const zerokv::Config& cfg);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zerokv
```

Update `include/zerokv/zerokv.h` to export it:

```cpp
#include "zerokv/message_kv.h"
```

Update `CMakeLists.txt` to compile/install the new files:

```cmake
set(ZEROKV_SOURCES
    ...
    src/message_kv.cpp
)
```

and ensure `include/zerokv/message_kv.h` is installed as part of the include tree.

- [ ] **Step 4: Add a minimal stub implementation**

Create `src/message_kv.cpp` with a skeletal implementation that compiles:

```cpp
#include "zerokv/message_kv.h"

#include <system_error>

namespace zerokv {

struct MessageKV::Impl {};

MessageKV::MessageKV(const zerokv::Config&) : impl_(std::make_unique<Impl>()) {}
MessageKV::~MessageKV() = default;

MessageKV::Ptr MessageKV::create(const zerokv::Config& cfg) {
    return Ptr(new MessageKV(cfg));
}

void MessageKV::start(const zerokv::kv::NodeConfig&) {
    throw std::system_error(make_error_code(ErrorCode::kUnimplemented));
}

void MessageKV::stop() {}

void MessageKV::send(const std::string&, const void*, size_t) {
    throw std::system_error(make_error_code(ErrorCode::kUnimplemented));
}

void MessageKV::send_region(const std::string&, const zerokv::MemoryRegion::Ptr&, size_t) {
    throw std::system_error(make_error_code(ErrorCode::kUnimplemented));
}

void MessageKV::recv(const std::string&,
                     const zerokv::MemoryRegion::Ptr&,
                     size_t,
                     size_t,
                     std::chrono::milliseconds) {
    throw std::system_error(make_error_code(ErrorCode::kUnimplemented));
}

MessageKV::BatchRecvResult MessageKV::recv_batch(
    const std::vector<BatchRecvItem>&,
    const zerokv::MemoryRegion::Ptr&,
    std::chrono::milliseconds) {
    throw std::system_error(make_error_code(ErrorCode::kUnimplemented));
}

}  // namespace zerokv
```

- [ ] **Step 5: Run the API-surface test and make sure it passes**

Run:

```bash
cmake --build build -j4 --target test_message_kv
ctest --test-dir build -R MessageKvApiSurfaceTest --output-on-failure
```

Expected:
- build succeeds
- the public type smoke test passes

- [ ] **Step 6: Commit**

```bash
git add include/zerokv/message_kv.h include/zerokv/zerokv.h src/message_kv.cpp CMakeLists.txt tests/integration/test_message_kv.cpp
git commit -m "Add MessageKV public API surface"
```

### Task 2: Implement Start/Stop and Internal State

**Files:**
- Modify: `src/message_kv.cpp`
- Test: `tests/integration/test_message_kv.cpp`

- [ ] **Step 1: Write failing lifecycle tests**

Extend `tests/integration/test_message_kv.cpp` with:

```cpp
class MessageKvIntegrationTest : public ::testing::Test {
protected:
    zerokv::Config cfg = zerokv::Config::builder().set_transport("tcp").build();
};

TEST_F(MessageKvIntegrationTest, StopBeforeStartIsSafe) {
    auto mq = zerokv::MessageKV::create(cfg);
    EXPECT_NO_THROW(mq->stop());
}

TEST_F(MessageKvIntegrationTest, StartAndStopAreIdempotentEnoughForSingleLifecycle) {
    auto server = zerokv::kv::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto mq = zerokv::MessageKV::create(cfg);
    EXPECT_NO_THROW(mq->start({server->address(), "127.0.0.1:0", "mq-node"}));
    EXPECT_NO_THROW(mq->stop());

    server->stop();
}
```

- [ ] **Step 2: Run the lifecycle tests and confirm they fail**

Run:

```bash
cmake --build build -j4 --target test_message_kv
ctest --test-dir build -R MessageKvIntegrationTest --output-on-failure
```

Expected:
- tests fail with `kUnimplemented`

- [ ] **Step 3: Implement wrapper state and lifecycle**

Replace the stub in `src/message_kv.cpp` with real state:

```cpp
struct MessageKV::Impl {
    zerokv::Config cfg;
    zerokv::kv::KVNode::Ptr node;
    std::mutex mu;
    bool running = false;

    explicit Impl(const zerokv::Config& config) : cfg(config) {}
};
```

Implement:

```cpp
MessageKV::MessageKV(const zerokv::Config& cfg) : impl_(std::make_unique<Impl>(cfg)) {}

void MessageKV::start(const zerokv::kv::NodeConfig& cfg) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->running) {
        return;
    }
    impl_->node = zerokv::kv::KVNode::create(impl_->cfg);
    auto status = impl_->node->start(cfg);
    status.throw_if_error();
    impl_->running = true;
}

void MessageKV::stop() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->running) {
        return;
    }
    impl_->node->stop();
    impl_->node.reset();
    impl_->running = false;
}
```

- [ ] **Step 4: Re-run lifecycle tests**

Run:

```bash
ctest --test-dir build -R MessageKvIntegrationTest --output-on-failure
```

Expected:
- lifecycle tests pass

- [ ] **Step 5: Commit**

```bash
git add src/message_kv.cpp tests/integration/test_message_kv.cpp
git commit -m "Implement MessageKV lifecycle"
```

### Task 3: Add Ack Naming and Cleanup Sweep Infrastructure

**Files:**
- Modify: `src/message_kv.cpp`
- Test: `tests/integration/test_message_kv.cpp`

- [ ] **Step 1: Write the failing internal cleanup behavior test**

Add a test that proves message cleanup is triggered by a later wrapper call:

```cpp
TEST_F(MessageKvIntegrationTest, SenderCleanupRunsOnSubsequentSend) {
    auto server = zerokv::kv::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto sender = zerokv::MessageKV::create(cfg);
    auto receiver = zerokv::MessageKV::create(cfg);
    sender->start({server->address(), "127.0.0.1:0", "sender"});
    receiver->start({server->address(), "127.0.0.1:0", "receiver"});

    auto rx_region = zerokv::MemoryRegion::allocate(16);
    ASSERT_NE(rx_region, nullptr);

    sender->send("msg-1", "hello", 5);
    receiver->recv("msg-1", rx_region, 5, 0, std::chrono::milliseconds(1000));
    sender->send("msg-2", "world", 5);

    auto keys = server->list_keys();
    EXPECT_THAT(keys, ::testing::Not(::testing::Contains("msg-1")));

    sender->stop();
    receiver->stop();
    server->stop();
}
```

- [ ] **Step 2: Run the cleanup behavior test and confirm it fails**

Run:

```bash
ctest --test-dir build -R SenderCleanupRunsOnSubsequentSend --output-on-failure
```

Expected:
- test fails because no ack or cleanup exists yet

- [ ] **Step 3: Add internal state, ack key convention, and cleanup sweep skeleton**

Implement in `src/message_kv.cpp`:

```cpp
namespace {

std::string make_ack_key(const std::string& message_key) {
    return "__message_kv_ack__:" + message_key;
}

struct PendingMessage {
    std::string message_key;
    std::string ack_key;
};

}  // namespace
```

Extend `Impl`:

```cpp
std::vector<PendingMessage> pending_messages;
std::vector<std::string> owned_ack_keys;
```

Add helper methods:

```cpp
void sweep_sender_cleanup_locked();
void sweep_receiver_ack_cleanup_locked();
void sweep_cleanup_locked();
```

The first pass should:
- iterate pending message entries
- use `impl_->node->wait_for_key(ack_key, std::chrono::milliseconds(0))` or a non-throwing lookup helper
- if ack exists, call `impl_->node->unpublish(message_key).get()`
- keep unresolved items pending

Receiver ack cleanup should:
- iterate `owned_ack_keys`
- attempt `unpublish(ack_key)`
- keep failed ones for later bounded retry

- [ ] **Step 4: Call cleanup sweep at the start and end of public methods**

Wrap each method under the internal mutex:

```cpp
sweep_cleanup_locked();
// do operation
sweep_cleanup_locked();
```

Do this for:
- `send`
- `send_region`
- `recv`
- `recv_batch`
- `stop` (bounded final sweep before stopping node)

- [ ] **Step 5: Re-run the cleanup behavior test**

Run:

```bash
ctest --test-dir build -R SenderCleanupRunsOnSubsequentSend --output-on-failure
```

Expected:
- test now passes

- [ ] **Step 6: Commit**

```bash
git add src/message_kv.cpp tests/integration/test_message_kv.cpp
git commit -m "Add MessageKV ack cleanup sweep infrastructure"
```

### Task 4: Implement Sender APIs

**Files:**
- Modify: `src/message_kv.cpp`
- Test: `tests/integration/test_message_kv.cpp`

- [ ] **Step 1: Write failing sender tests**

Add:

```cpp
TEST_F(MessageKvIntegrationTest, SendPublishesMessageKey) {
    auto server = zerokv::kv::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto sender = zerokv::MessageKV::create(cfg);
    sender->start({server->address(), "127.0.0.1:0", "sender"});

    sender->send("msg-key", "payload", 7);

    auto info = server->lookup("msg-key");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->size, 7u);

    sender->stop();
    server->stop();
}

TEST_F(MessageKvIntegrationTest, SendRegionPublishesMessageKey) {
    auto server = zerokv::kv::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto sender = zerokv::MessageKV::create(cfg);
    sender->start({server->address(), "127.0.0.1:0", "sender"});

    auto region = zerokv::MemoryRegion::allocate(8);
    ASSERT_NE(region, nullptr);
    std::memcpy(region->address(), "12345678", 8);

    sender->send_region("msg-region", region, 8);

    auto info = server->lookup("msg-region");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->size, 8u);

    sender->stop();
    server->stop();
}
```

- [ ] **Step 2: Run the sender tests and confirm they fail**

Run:

```bash
ctest --test-dir build -R 'SendPublishesMessageKey|SendRegionPublishesMessageKey' --output-on-failure
```

Expected:
- tests fail while sender methods are still unimplemented

- [ ] **Step 3: Implement `send()` and `send_region()`**

Implement blocking sender methods:

```cpp
void MessageKV::send(const std::string& key, const void* data, size_t size) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->running) {
        Status(ErrorCode::kConnectionRefused, "MessageKV is not running").throw_if_error();
    }
    if (key.empty()) {
        Status(ErrorCode::kInvalidArgument, "message key must not be empty").throw_if_error();
    }
    if (size > 0 && data == nullptr) {
        Status(ErrorCode::kInvalidArgument, "message data is required").throw_if_error();
    }

    sweep_cleanup_locked();
    auto publish = impl_->node->publish(key, data, size);
    publish.status().throw_if_error();
    publish.get();
    publish.status().throw_if_error();
    impl_->pending_messages.push_back({key, make_ack_key(key)});
    sweep_cleanup_locked();
}
```

Implement `send_region()` analogously, validating `region` and `size <= region->length()`.

- [ ] **Step 4: Re-run the sender tests**

Run:

```bash
ctest --test-dir build -R 'SendPublishesMessageKey|SendRegionPublishesMessageKey' --output-on-failure
```

Expected:
- both sender tests pass

- [ ] **Step 5: Commit**

```bash
git add src/message_kv.cpp tests/integration/test_message_kv.cpp
git commit -m "Implement MessageKV sender APIs"
```

### Task 5: Implement Receiver APIs and Ack Publication

**Files:**
- Modify: `src/message_kv.cpp`
- Test: `tests/integration/test_message_kv.cpp`

- [ ] **Step 1: Write failing receiver tests**

Add:

```cpp
TEST_F(MessageKvIntegrationTest, RecvCopiesSingleMessageIntoRegion) {
    auto server = zerokv::kv::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto sender = zerokv::MessageKV::create(cfg);
    auto receiver = zerokv::MessageKV::create(cfg);
    sender->start({server->address(), "127.0.0.1:0", "sender"});
    receiver->start({server->address(), "127.0.0.1:0", "receiver"});

    auto region = zerokv::MemoryRegion::allocate(8);
    ASSERT_NE(region, nullptr);

    sender->send("rx-key", "payload", 7);
    receiver->recv("rx-key", region, 7, 0, std::chrono::milliseconds(1000));

    EXPECT_EQ(std::memcmp(region->address(), "payload", 7), 0);

    sender->stop();
    receiver->stop();
    server->stop();
}

TEST_F(MessageKvIntegrationTest, RecvBatchReturnsPartialTimeout) {
    auto server = zerokv::kv::KVServer::create(cfg);
    ASSERT_TRUE(server->start({"127.0.0.1:0"}).ok());

    auto sender = zerokv::MessageKV::create(cfg);
    auto receiver = zerokv::MessageKV::create(cfg);
    sender->start({server->address(), "127.0.0.1:0", "sender"});
    receiver->start({server->address(), "127.0.0.1:0", "receiver"});

    auto region = zerokv::MemoryRegion::allocate(16);
    ASSERT_NE(region, nullptr);

    sender->send("batch-a", "aaaa", 4);
    auto result = receiver->recv_batch({
        {"batch-a", 4, 0},
        {"batch-b", 4, 8},
    }, region, std::chrono::milliseconds(100));

    EXPECT_THAT(result.completed, ::testing::ElementsAre("batch-a"));
    EXPECT_THAT(result.timed_out, ::testing::ElementsAre("batch-b"));
    EXPECT_FALSE(result.completed_all);

    sender->stop();
    receiver->stop();
    server->stop();
}
```

- [ ] **Step 2: Run the receiver tests and confirm they fail**

Run:

```bash
ctest --test-dir build -R 'RecvCopiesSingleMessageIntoRegion|RecvBatchReturnsPartialTimeout' --output-on-failure
```

Expected:
- tests fail while receiver methods are unimplemented

- [ ] **Step 3: Implement ack publication helper**

Add an internal helper:

```cpp
void publish_ack_locked(const std::string& message_key) {
    const auto ack_key = make_ack_key(message_key);
    auto publish = impl_->node->publish(ack_key, "", 0);
    publish.status().throw_if_error();
    publish.get();
    publish.status().throw_if_error();
    impl_->owned_ack_keys.push_back(ack_key);
}
```

- [ ] **Step 4: Implement `recv_batch()` and `recv()`**

Implement `recv_batch()` by mapping to `KVNode::subscribe_and_fetch_to_once_many(...)`:

```cpp
MessageKV::BatchRecvResult MessageKV::recv_batch(
    const std::vector<BatchRecvItem>& items,
    const zerokv::MemoryRegion::Ptr& region,
    std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->running) {
        Status(ErrorCode::kConnectionRefused, "MessageKV is not running").throw_if_error();
    }

    sweep_cleanup_locked();

    std::vector<zerokv::kv::FetchToItem> kv_items;
    kv_items.reserve(items.size());
    for (const auto& item : items) {
        kv_items.push_back({item.key, item.length, item.offset});
    }

    auto batch = impl_->node->subscribe_and_fetch_to_once_many(kv_items, region, timeout);

    for (const auto& key : batch.completed) {
        publish_ack_locked(key);
    }

    BatchRecvResult result;
    result.completed = std::move(batch.completed);
    result.failed = std::move(batch.failed);
    result.timed_out = std::move(batch.timed_out);
    result.completed_all = batch.completed_all;

    sweep_cleanup_locked();
    return result;
}
```

Implement `recv()` as a one-item wrapper that throws on any non-success:

```cpp
void MessageKV::recv(...) {
    auto result = recv_batch({BatchRecvItem{key, length, offset}}, region, timeout);
    if (!result.completed_all) {
        Status(ErrorCode::kTimeout, "message receive did not complete").throw_if_error();
    }
}
```

- [ ] **Step 5: Re-run the receiver tests**

Run:

```bash
ctest --test-dir build -R 'RecvCopiesSingleMessageIntoRegion|RecvBatchReturnsPartialTimeout' --output-on-failure
```

Expected:
- both receiver tests pass

- [ ] **Step 6: Commit**

```bash
git add src/message_kv.cpp tests/integration/test_message_kv.cpp
git commit -m "Implement MessageKV receiver APIs"
```

### Task 6: Finish Lifecycle, Validation, and Documentation

**Files:**
- Modify: `src/message_kv.cpp`
- Modify: `README.md`
- Modify: `docs/reports/zerokv-rdma-kv-mvp.md`
- Test: `tests/integration/test_message_kv.cpp`

- [ ] **Step 1: Add failing validation and stop-behavior tests**

Add:

```cpp
TEST_F(MessageKvIntegrationTest, SendRejectsEmptyKey) {
    auto mq = zerokv::MessageKV::create(cfg);
    EXPECT_THROW(mq->send("", "x", 1), std::system_error);
}

TEST_F(MessageKvIntegrationTest, RecvBatchRejectsInvalidLayoutBeforeWaiting) {
    auto mq = zerokv::MessageKV::create(cfg);
    auto region = zerokv::MemoryRegion::allocate(8);
    ASSERT_NE(region, nullptr);
    EXPECT_THROW((void)mq->recv_batch({
        {"a", 8, 0},
        {"b", 8, 4},
    }, region, std::chrono::milliseconds(1)), std::system_error);
}
```

- [ ] **Step 2: Run the validation tests and confirm failure if behavior is missing**

Run:

```bash
ctest --test-dir build -R 'SendRejectsEmptyKey|RecvBatchRejectsInvalidLayoutBeforeWaiting' --output-on-failure
```

Expected:
- tests fail until validation paths are complete

- [ ] **Step 3: Finish validation and bounded stop cleanup**

In `src/message_kv.cpp`:

- reject empty keys in `send`, `send_region`, and `recv`
- reject invalid region/size in sender APIs
- let `recv_batch` reuse `subscribe_and_fetch_to_once_many` layout validation by building `kv::FetchToItem`
- cap stop cleanup to a single bounded sweep:

```cpp
void MessageKV::stop() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->running) {
        return;
    }
    sweep_cleanup_locked();
    impl_->node->stop();
    impl_->node.reset();
    impl_->running = false;
}
```

- [ ] **Step 4: Document the wrapper**

Add a short section to `README.md`:

```md
## MessageKV

`zerokv::MessageKV` is a scenario-oriented wrapper for message-style workflows
where business code already manages unique keys.

- `send()` / `send_region()` publish message keys
- `recv()` / `recv_batch()` wait for message keys and fetch zero-copy into caller memory
- internal ack markers drive later sender-side cleanup
```

Add a short status note to `docs/reports/zerokv-rdma-kv-mvp.md` describing Phase 1:

```md
- `MessageKV` Phase 1 is implemented on top of publish/publish_region and
  subscribe_and_fetch_to_once_many.
- Internal ack markers are used for best-effort sender-side reclamation.
```

- [ ] **Step 5: Run the focused test suite**

Run:

```bash
cmake --build build -j4 --target test_message_kv
ctest --test-dir build -R 'MessageKv' --output-on-failure
```

Expected:
- all MessageKV integration tests pass

- [ ] **Step 6: Commit**

```bash
git add src/message_kv.cpp README.md docs/reports/zerokv-rdma-kv-mvp.md tests/integration/test_message_kv.cpp
git commit -m "Complete MessageKV phase 1 wrapper"
```

## Self-Review

- Spec coverage:
  - wrapper API: Task 1
  - start/stop lifecycle: Task 2
  - internal ack-based cleanup without background KV thread: Task 3
  - blocking sender APIs: Task 4
  - receiver zero-copy fetch path and partial timeout behavior: Task 5
  - lifecycle/validation/docs: Task 6
- Placeholder scan:
  - no `TODO`, `TBD`, or undefined helper references remain
- Type consistency:
  - `MessageKV`, `BatchRecvItem`, `BatchRecvResult`, `make_ack_key`, and `sweep_cleanup_locked` are introduced before later tasks rely on them
