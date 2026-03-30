# KV Wait And Fetch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add client-side helpers that wait for keys to appear and fetch them, including a batch helper that fetches each key as soon as it becomes ready and returns once the whole batch completes or times out.

**Architecture:** Keep all logic inside `KVNode` and reuse the existing lookup, subscription, event draining, and fetch paths. Do not add server protocol. Implement synchronous orchestration helpers with a small internal polling loop, first-success-wins semantics, and cleanup that preserves user-owned subscriptions.

**Tech Stack:** C++17, existing AXON `KVNode`/`KVServer` APIs, GoogleTest integration tests.

---

## File map

- Modify: `include/axon/kv.h`
  - Add `WaitKeysResult`, `BatchFetchResult`, and the new synchronous `KVNode` helper methods.
- Modify: `src/kv/node.cpp`
  - Implement the new helpers and small internal utility code for deduplication, lookup polling, subscription ownership tracking, and batch orchestration.
- Modify: `tests/integration/test_kv_node.cpp`
  - Add end-to-end tests for single-key wait/fetch, batch readiness, partial timeout results, duplicate handling, and first-success-wins behavior.

### Task 1: Public API and the first failing tests

**Files:**
- Modify: `include/axon/kv.h`
- Test: `tests/integration/test_kv_node.cpp`

- [ ] **Step 1: Add the new public API declarations**

Add these declarations to `include/axon/kv.h` near the existing `FetchResult` and `KVNode` methods:

```cpp
struct WaitKeysResult {
    std::vector<std::string> ready;
    std::vector<std::string> timed_out;
    bool completed = false;
};

struct BatchFetchResult {
    std::vector<std::pair<std::string, FetchResult>> fetched;
    std::vector<std::string> failed;
    std::vector<std::string> timed_out;
    bool completed = false;
};
```

```cpp
Status wait_for_key(const std::string& key,
                    std::chrono::milliseconds timeout);

WaitKeysResult wait_for_keys(const std::vector<std::string>& keys,
                             std::chrono::milliseconds timeout);

FetchResult subscribe_and_fetch_once(const std::string& key,
                                     std::chrono::milliseconds timeout);

BatchFetchResult subscribe_and_fetch_once_many(
    const std::vector<std::string>& keys,
    std::chrono::milliseconds timeout);
```

- [ ] **Step 2: Write the first two failing integration tests**

Append these tests to `tests/integration/test_kv_node.cpp`:

```cpp
TEST(KvNodeIntegrationTest, WaitForKeyReturnsWhenKeyAppears) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto waiter = KVNode::create(cfg);
    auto publisher = KVNode::create(cfg);
    ASSERT_TRUE(waiter->start(NodeConfig{server->address(), "127.0.0.1:0", "waiter-a"}).ok());
    ASSERT_TRUE(publisher->start(NodeConfig{server->address(), "127.0.0.1:0", "publisher-a"}).ok());

    std::thread publish_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::string value = "ready-value";
        auto publish = publisher->publish("wait-key", value.data(), value.size());
        ASSERT_TRUE(publish.status().ok());
        publish.get();
    });

    auto status = waiter->wait_for_key("wait-key", std::chrono::milliseconds(1000));
    EXPECT_TRUE(status.ok()) << status.message();

    publish_thread.join();
    publisher->stop();
    waiter->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, SubscribeAndFetchOnceFetchesPublishedKey) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();

    auto server = KVServer::create(cfg);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto waiter = KVNode::create(cfg);
    auto publisher = KVNode::create(cfg);
    ASSERT_TRUE(waiter->start(NodeConfig{server->address(), "127.0.0.1:0", "waiter-b"}).ok());
    ASSERT_TRUE(publisher->start(NodeConfig{server->address(), "127.0.0.1:0", "publisher-b"}).ok());

    std::thread publish_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::string value = "fetch-once-value";
        auto publish = publisher->publish("fetch-once-key", value.data(), value.size());
        ASSERT_TRUE(publish.status().ok());
        publish.get();
    });

    auto result = waiter->subscribe_and_fetch_once("fetch-once-key", std::chrono::milliseconds(1000));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(result.data.data()), result.data.size()), "fetch-once-value");
    EXPECT_EQ(result.owner_node_id, "publisher-b");
    EXPECT_EQ(result.version, 1u);

    publish_thread.join();
    publisher->stop();
    waiter->stop();
    server->stop();
}
```

- [ ] **Step 3: Run the targeted test to verify compile or link failure**

Run:

```bash
cmake --build build --target test_kv_node -j4
```

Expected:
- compile failure because the new `KVNode` methods are declared but not defined yet

- [ ] **Step 4: Commit the API + failing tests**

```bash
git add include/axon/kv.h tests/integration/test_kv_node.cpp
git commit -m "Add wait-and-fetch API declarations and tests"
```

### Task 2: Single-key wait/fetch implementation

**Files:**
- Modify: `src/kv/node.cpp`
- Test: `tests/integration/test_kv_node.cpp`

- [ ] **Step 1: Add small internal helpers in `src/kv/node.cpp`**

Add focused internal helpers near the existing anonymous-namespace utilities:

```cpp
std::vector<std::string> dedupe_keys(const std::vector<std::string>& keys);

bool key_exists(const KVNode::Impl* impl, const std::string& key);

std::optional<FetchResult> try_fetch_now(KVNode::Impl* impl, const std::string& key);
```

Use `std::vector<std::string>` + sort/unique for deterministic ordering.

- [ ] **Step 2: Implement `wait_for_key` and `subscribe_and_fetch_once` minimally**

Implement synchronous logic in `src/kv/node.cpp`:

```cpp
Status KVNode::wait_for_key(const std::string& key, std::chrono::milliseconds timeout) {
    if (!impl_ || !impl_->running_.load()) {
        return Status(ErrorCode::kConnectionRefused, "KVNode is not running");
    }
    if (key.empty()) {
        return Status(ErrorCode::kInvalidArgument, "key is required");
    }

    if (key_exists(impl_.get(), key)) {
        return Status::OK();
    }

    auto subscribe = this->subscribe(key);
    if (!subscribe.status().ok()) {
        return subscribe.status();
    }
    subscribe.get();

    const auto deadline = SteadyClock::now() + timeout;
    while (SteadyClock::now() < deadline) {
        if (key_exists(impl_.get(), key)) {
            auto unsubscribe = this->unsubscribe(key);
            if (unsubscribe.status().ok()) {
                unsubscribe.get();
            }
            return Status::OK();
        }

        auto events = drain_subscription_events();
        for (const auto& event : events) {
            if (event.key == key && (event.type == SubscriptionEventType::kPublished ||
                                     event.type == SubscriptionEventType::kUpdated)) {
                if (key_exists(impl_.get(), key)) {
                    auto unsubscribe = this->unsubscribe(key);
                    if (unsubscribe.status().ok()) {
                        unsubscribe.get();
                    }
                    return Status::OK();
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto unsubscribe = this->unsubscribe(key);
    if (unsubscribe.status().ok()) {
        unsubscribe.get();
    }
    return Status(ErrorCode::kTimeout, "timed out waiting for key");
}
```

```cpp
FetchResult KVNode::subscribe_and_fetch_once(const std::string& key,
                                             std::chrono::milliseconds timeout) {
    auto status = wait_for_key(key, timeout);
    status.throw_if_error();

    auto fetch_future = fetch(key);
    fetch_future.status().throw_if_error();
    return fetch_future.get();
}
```

- [ ] **Step 3: Build and run the two new tests**

Run:

```bash
cmake --build build --target test_kv_node -j4
./build/test_kv_node --gtest_filter='KvNodeIntegrationTest.WaitForKeyReturnsWhenKeyAppears:KvNodeIntegrationTest.SubscribeAndFetchOnceFetchesPublishedKey'
```

Expected:
- both tests PASS

- [ ] **Step 4: Commit the single-key implementation**

```bash
git add src/kv/node.cpp tests/integration/test_kv_node.cpp
 git commit -m "Implement single-key wait and fetch helpers"
```

### Task 3: Batch wait/fetch orchestration

**Files:**
- Modify: `src/kv/node.cpp`
- Test: `tests/integration/test_kv_node.cpp`

- [ ] **Step 1: Add failing batch tests**

Append these tests:

```cpp
TEST(KvNodeIntegrationTest, WaitForKeysReturnsWhenAllKeysAppear) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();
    auto server = KVServer::create(cfg);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto waiter = KVNode::create(cfg);
    auto publisher = KVNode::create(cfg);
    ASSERT_TRUE(waiter->start(NodeConfig{server->address(), "127.0.0.1:0", "waiter-c"}).ok());
    ASSERT_TRUE(publisher->start(NodeConfig{server->address(), "127.0.0.1:0", "publisher-c"}).ok());

    std::thread publish_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::string a = "value-a";
        auto first = publisher->publish("batch-a", a.data(), a.size());
        ASSERT_TRUE(first.status().ok());
        first.get();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::string b = "value-b";
        auto second = publisher->publish("batch-b", b.data(), b.size());
        ASSERT_TRUE(second.status().ok());
        second.get();
    });

    auto result = waiter->wait_for_keys({"batch-a", "batch-b"}, std::chrono::milliseconds(1000));
    EXPECT_TRUE(result.completed);
    EXPECT_EQ(result.ready.size(), 2u);
    EXPECT_TRUE(result.timed_out.empty());

    publish_thread.join();
    publisher->stop();
    waiter->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, SubscribeAndFetchOnceManyFetchesEachKeyWhenReady) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();
    auto server = KVServer::create(cfg);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto waiter = KVNode::create(cfg);
    auto publisher = KVNode::create(cfg);
    ASSERT_TRUE(waiter->start(NodeConfig{server->address(), "127.0.0.1:0", "waiter-d"}).ok());
    ASSERT_TRUE(publisher->start(NodeConfig{server->address(), "127.0.0.1:0", "publisher-d"}).ok());

    std::thread publish_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const std::string a = "value-a";
        auto first = publisher->publish("fetch-a", a.data(), a.size());
        ASSERT_TRUE(first.status().ok());
        first.get();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        const std::string b = "value-b";
        auto second = publisher->publish("fetch-b", b.data(), b.size());
        ASSERT_TRUE(second.status().ok());
        second.get();
    });

    auto result = waiter->subscribe_and_fetch_once_many({"fetch-a", "fetch-b"}, std::chrono::milliseconds(1000));
    EXPECT_TRUE(result.completed);
    EXPECT_EQ(result.fetched.size(), 2u);
    EXPECT_TRUE(result.failed.empty());
    EXPECT_TRUE(result.timed_out.empty());

    publish_thread.join();
    publisher->stop();
    waiter->stop();
    server->stop();
}
```

- [ ] **Step 2: Implement `wait_for_keys` and `subscribe_and_fetch_once_many`**

Add an internal synchronous batch loop in `src/kv/node.cpp`:

```cpp
WaitKeysResult KVNode::wait_for_keys(const std::vector<std::string>& keys,
                                     std::chrono::milliseconds timeout) {
    WaitKeysResult result;
    if (!impl_ || !impl_->running_.load()) {
        return result;
    }

    auto deduped = dedupe_keys(keys);
    if (deduped.empty()) {
        return result;
    }

    std::unordered_set<std::string> pending;
    std::vector<std::string> subscribed_here;
    for (const auto& key : deduped) {
        if (key_exists(impl_.get(), key)) {
            result.ready.push_back(key);
        } else {
            pending.insert(key);
            auto subscribe = this->subscribe(key);
            subscribe.status().throw_if_error();
            subscribe.get();
            subscribed_here.push_back(key);
        }
    }

    const auto deadline = SteadyClock::now() + timeout;
    auto next_lookup = SteadyClock::now();
    while (!pending.empty() && SteadyClock::now() < deadline) {
        auto events = drain_subscription_events();
        for (const auto& event : events) {
            if (!pending.count(event.key)) {
                continue;
            }
            if (event.type == SubscriptionEventType::kPublished ||
                event.type == SubscriptionEventType::kUpdated) {
                if (key_exists(impl_.get(), event.key)) {
                    pending.erase(event.key);
                    result.ready.push_back(event.key);
                }
            }
        }

        if (SteadyClock::now() >= next_lookup) {
            std::vector<std::string> completed;
            for (const auto& key : pending) {
                if (key_exists(impl_.get(), key)) {
                    completed.push_back(key);
                }
            }
            for (const auto& key : completed) {
                pending.erase(key);
                result.ready.push_back(key);
            }
            next_lookup = SteadyClock::now() + std::chrono::milliseconds(100);
        }

        if (!pending.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    result.timed_out.assign(pending.begin(), pending.end());
    result.completed = pending.empty();

    for (const auto& key : subscribed_here) {
        auto unsubscribe = this->unsubscribe(key);
        if (unsubscribe.status().ok()) {
            unsubscribe.get();
        }
    }
    return result;
}
```

Implement `subscribe_and_fetch_once_many` using the same pending set plus:
- immediate fetch for already-present keys
- event-driven fetch attempts for published/updated keys
- periodic lookup fallback every 100ms
- `fetched` on success
- `timed_out` for keys still pending at deadline
- `failed` only for keys that hit a terminal non-timeout condition

- [ ] **Step 3: Run the four wait/fetch tests**

Run:

```bash
./build/test_kv_node --gtest_filter='KvNodeIntegrationTest.WaitForKeyReturnsWhenKeyAppears:KvNodeIntegrationTest.SubscribeAndFetchOnceFetchesPublishedKey:KvNodeIntegrationTest.WaitForKeysReturnsWhenAllKeysAppear:KvNodeIntegrationTest.SubscribeAndFetchOnceManyFetchesEachKeyWhenReady'
```

Expected:
- all four PASS

- [ ] **Step 4: Commit the batch orchestration**

```bash
git add src/kv/node.cpp tests/integration/test_kv_node.cpp
git commit -m "Implement batch wait-and-fetch helpers"
```

### Task 4: Timeout, deduplication, and first-success-wins tests

**Files:**
- Modify: `tests/integration/test_kv_node.cpp`
- Modify: `src/kv/node.cpp`

- [ ] **Step 1: Add failing edge-case tests**

Append these tests:

```cpp
TEST(KvNodeIntegrationTest, SubscribeAndFetchOnceManyReturnsPartialResultsOnTimeout) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();
    auto server = KVServer::create(cfg);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto waiter = KVNode::create(cfg);
    auto publisher = KVNode::create(cfg);
    ASSERT_TRUE(waiter->start(NodeConfig{server->address(), "127.0.0.1:0", "waiter-e"}).ok());
    ASSERT_TRUE(publisher->start(NodeConfig{server->address(), "127.0.0.1:0", "publisher-e"}).ok());

    std::thread publish_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::string value = "partial-value";
        auto publish = publisher->publish("partial-a", value.data(), value.size());
        ASSERT_TRUE(publish.status().ok());
        publish.get();
    });

    auto result = waiter->subscribe_and_fetch_once_many({"partial-a", "partial-b"}, std::chrono::milliseconds(200));
    EXPECT_FALSE(result.completed);
    EXPECT_EQ(result.fetched.size(), 1u);
    EXPECT_TRUE(result.failed.empty());
    EXPECT_EQ(result.timed_out.size(), 1u);

    publish_thread.join();
    publisher->stop();
    waiter->stop();
    server->stop();
}

TEST(KvNodeIntegrationTest, SubscribeAndFetchOnceManyDeduplicatesKeys) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();
    auto server = KVServer::create(cfg);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto waiter = KVNode::create(cfg);
    auto publisher = KVNode::create(cfg);
    ASSERT_TRUE(waiter->start(NodeConfig{server->address(), "127.0.0.1:0", "waiter-f"}).ok());
    ASSERT_TRUE(publisher->start(NodeConfig{server->address(), "127.0.0.1:0", "publisher-f"}).ok());

    const std::string value = "dup-value";
    auto publish = publisher->publish("dup-key", value.data(), value.size());
    ASSERT_TRUE(publish.status().ok());
    publish.get();

    auto result = waiter->subscribe_and_fetch_once_many({"dup-key", "dup-key"}, std::chrono::milliseconds(500));
    EXPECT_TRUE(result.completed);
    EXPECT_EQ(result.fetched.size(), 1u);

    publisher->stop();
    waiter->stop();
    server->stop();
}
```

- [ ] **Step 2: Add a first-success-wins regression test**

Append this test:

```cpp
TEST(KvNodeIntegrationTest, SubscribeAndFetchOnceManyLocksFirstSuccessfulVersion) {
    auto cfg = axon::Config::builder().set_transport("tcp").build();
    auto server = KVServer::create(cfg);
    ASSERT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    auto waiter = KVNode::create(cfg);
    auto publisher = KVNode::create(cfg);
    ASSERT_TRUE(waiter->start(NodeConfig{server->address(), "127.0.0.1:0", "waiter-g"}).ok());
    ASSERT_TRUE(publisher->start(NodeConfig{server->address(), "127.0.0.1:0", "publisher-g"}).ok());

    std::thread publish_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const std::string first = "version-one";
        auto p1 = publisher->publish("versioned-key", first.data(), first.size());
        ASSERT_TRUE(p1.status().ok());
        p1.get();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const std::string second = "version-two";
        auto p2 = publisher->publish("versioned-key", second.data(), second.size());
        ASSERT_TRUE(p2.status().ok());
        p2.get();
    });

    auto result = waiter->subscribe_and_fetch_once_many({"versioned-key"}, std::chrono::milliseconds(1000));
    ASSERT_EQ(result.fetched.size(), 1u);
    EXPECT_EQ(result.fetched[0].second.version, 1u);

    publish_thread.join();
    publisher->stop();
    waiter->stop();
    server->stop();
}
```

- [ ] **Step 3: Implement the minimal edge-case handling**

In `src/kv/node.cpp`:
- reject empty key / empty batch with `Status(ErrorCode::kInvalidArgument, ...)` or `throw` via `Status::throw_if_error()` in synchronous helpers
- ensure dedupe is used everywhere
- ensure a key that has already succeeded is removed from the pending set and ignored by later events
- ensure timeout keys remain only in `timed_out`

- [ ] **Step 4: Run the full KV node integration suite**

Run:

```bash
ctest -R IntegrationKvNode --output-on-failure
```

Expected:
- PASS

- [ ] **Step 5: Commit the edge-case fixes**

```bash
git add src/kv/node.cpp tests/integration/test_kv_node.cpp
 git commit -m "Cover wait-and-fetch edge cases"
```

### Task 5: Final verification and documentation note

**Files:**
- Modify: `docs/reports/axon-rdma-kv-mvp.md`

- [ ] **Step 1: Add a short MVP+1 note to the report**

Append a brief note to `docs/reports/axon-rdma-kv-mvp.md` describing the new helpers:

```markdown
### Wait-and-fetch helpers

`KVNode` now exposes synchronous helpers for workflows where a consumer needs to
subscribe to keys before they exist and fetch them as they become available.

- `wait_for_key` / `wait_for_keys`
- `subscribe_and_fetch_once`
- `subscribe_and_fetch_once_many`

The batch helper fetches each key as soon as it becomes ready and returns once
all keys succeed or the timeout expires.
```

- [ ] **Step 2: Run final verification**

Run:

```bash
cmake --build build --target test_kv_node -j4
ctest -R IntegrationKvNode --output-on-failure
```

Expected:
- build succeeds
- `IntegrationKvNode` passes

- [ ] **Step 3: Commit final polish**

```bash
git add docs/reports/axon-rdma-kv-mvp.md
 git commit -m "Document wait-and-fetch helpers"
```
