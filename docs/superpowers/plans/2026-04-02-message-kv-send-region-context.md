# MessageKV Send Region Context Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a safe `MessageKV` send-region allocation path that uses the internal `KVNode` context, then switch `message_kv_demo` to that path so RDMA `send_region()` works correctly.

**Architecture:** Keep `send()` and `send_region()` unchanged. Add a narrow `KVNode::allocate_region(size)` helper and a public `MessageKV::allocate_send_region(size)` wrapper that allocates registered memory from the internal node context. Then update the demo sender to allocate reusable per-thread regions from `MessageKV` itself rather than from an external `Context`.

**Tech Stack:** C++20, ZeroKV `MessageKV`, `KVNode`, `MemoryRegion`, GoogleTest, VM1/VM2 TCP+RDMA validation

---

## File Map

- Modify: `include/zerokv/kv.h`
  - Declare `KVNode::allocate_region(size_t)`.
- Modify: `src/kv/node.cpp`
  - Implement `KVNode::allocate_region(size_t)` using `impl_->context_`.
- Modify: `include/zerokv/message_kv.h`
  - Declare `MessageKV::allocate_send_region(size_t)`.
- Modify: `src/message_kv.cpp`
  - Implement `allocate_send_region(size_t)` with start-state and allocation-failure handling.
- Modify: `examples/message_kv_demo.cpp`
  - Remove external sender `Context` creation; allocate reusable send regions from `MessageKV`.
- Modify: `tests/integration/test_message_kv.cpp`
  - Add `allocate_send_region()` start/stop behavior tests.
- Verify: `tests/integration/test_message_kv_demo.cpp`
  - Existing helper tests should still pass unchanged.

### Task 1: Add narrow region allocation helpers to KVNode and MessageKV

**Files:**
- Modify: `include/zerokv/kv.h`
- Modify: `src/kv/node.cpp`
- Modify: `include/zerokv/message_kv.h`
- Modify: `src/message_kv.cpp`
- Test: `tests/integration/test_message_kv.cpp`

- [ ] **Step 1: Add the public declarations**

Add to `include/zerokv/kv.h`:

```cpp
[[nodiscard]] zerokv::MemoryRegion::Ptr allocate_region(size_t size) const;
```

Add to `include/zerokv/message_kv.h`:

```cpp
[[nodiscard]] zerokv::MemoryRegion::Ptr allocate_send_region(size_t size);
```

Keep comments brief and explicit: these allocate registered send buffers from the internal node context.

- [ ] **Step 2: Write the failing MessageKV tests first**

Add tests in `tests/integration/test_message_kv.cpp`:

```cpp
TEST_F(MessageKvIntegrationTest, AllocateSendRegionRequiresRunningNode) {
    auto mq = zerokv::MessageKV::create(cfg_);
    expect_system_error_code([&] { (void)mq->allocate_send_region(1024); },
                             zerokv::ErrorCode::kConnectionRefused);
}

TEST_F(MessageKvIntegrationTest, AllocateSendRegionSucceedsAfterStart) {
    auto mq = zerokv::MessageKV::create(cfg_);
    mq->start(sender_cfg_);
    auto region = mq->allocate_send_region(1024);
    ASSERT_NE(region, nullptr);
    EXPECT_GE(region->length(), 1024u);
    mq->stop();
}
```

Use the existing fixture/config helpers in this file; do not invent new fixtures.

- [ ] **Step 3: Run the targeted test filter and confirm failure**

Run:
```bash
./build/test_message_kv --gtest_filter="MessageKvIntegrationTest.AllocateSendRegionRequiresRunningNode:MessageKvIntegrationTest.AllocateSendRegionSucceedsAfterStart"
```
Expected: compile or test failure because the new methods do not exist yet.

- [ ] **Step 4: Implement `KVNode::allocate_region(size)` minimally**

In `src/kv/node.cpp`, implement:

```cpp
zerokv::MemoryRegion::Ptr KVNode::allocate_region(size_t size) const {
    if (!impl_) {
        return nullptr;
    }
    return MemoryRegion::allocate(impl_->context_, size);
}
```

Do not add extra policy here. This is a narrow passthrough.

- [ ] **Step 5: Implement `MessageKV::allocate_send_region(size)` minimally**

In `src/message_kv.cpp`, implement:

```cpp
zerokv::MemoryRegion::Ptr MessageKV::allocate_send_region(size_t size) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->node_ready_locked()) {
        throw std::system_error(make_error_code(ErrorCode::kConnectionRefused));
    }
    auto region = impl_->node->allocate_region(size);
    if (!region) {
        throw std::system_error(make_error_code(ErrorCode::kRegistrationFailed));
    }
    return region;
}
```

Do not change send/recv semantics.

- [ ] **Step 6: Run targeted tests and the existing core MessageKV smoke**

Run:
```bash
./build/test_message_kv --gtest_filter="MessageKvIntegrationTest.AllocateSendRegionRequiresRunningNode:MessageKvIntegrationTest.AllocateSendRegionSucceedsAfterStart:MessageKvIntegrationTest.RecvCopiesSingleMessageIntoRegion:MessageKvIntegrationTest.SendUnpublishesMessageKeyAfterAck"
```
Expected: all pass.

- [ ] **Step 7: Commit Task 1**

```bash
git add include/zerokv/kv.h src/kv/node.cpp include/zerokv/message_kv.h src/message_kv.cpp tests/integration/test_message_kv.cpp
git commit -m "Add MessageKV send region allocation helper"
```

### Task 2: Switch the demo sender to allocate regions from MessageKV

**Files:**
- Modify: `examples/message_kv_demo.cpp`
- Verify: `tests/integration/test_message_kv_demo.cpp`

- [ ] **Step 1: Remove external sender `Context` ownership from the demo**

Delete the sender-side vectors and initialization that are only there to create external contexts:

```cpp
std::vector<Context::Ptr> workers_ctx(...);
```

and replace sender setup with just:

```cpp
std::vector<MemoryRegion::Ptr> workers_region(static_cast<size_t>(args.threads));
std::vector<MessageKV::Ptr> workers_mq(static_cast<size_t>(args.threads));
```

- [ ] **Step 2: Allocate reusable sender regions from `MessageKV` after `start()`**

In the sender setup loop, after `workers_mq[i]->start(...)`, allocate:

```cpp
workers_region[static_cast<size_t>(i)] =
    workers_mq[static_cast<size_t>(i)]->allocate_send_region(max_size_bytes);
```

The thread worker lambda should keep using:

```cpp
auto& region = workers_region[static_cast<size_t>(i)];
std::memcpy(region->address(), payload.data(), size_bytes);
mq->send_region(key, region, size_bytes);
```

- [ ] **Step 3: Rebuild demo helper target and run helper tests**

Run:
```bash
cmake --build build -j4 --target test_message_kv_demo message_kv_demo
./build/test_message_kv_demo
```
Expected: build succeeds and all helper tests pass.

- [ ] **Step 4: Commit Task 2**

```bash
git add examples/message_kv_demo.cpp
git commit -m "Use MessageKV-owned send regions in demo"
```

### Task 3: Validate TCP and RDMA on VM1/VM2

**Files:**
- Verify only: `examples/message_kv_demo.cpp`, `README.md`, `tests/integration/test_message_kv.cpp`

- [ ] **Step 1: Sync the changed files to VM1 and VM2 and rebuild**

Copy:
- `include/zerokv/kv.h`
- `src/kv/node.cpp`
- `include/zerokv/message_kv.h`
- `src/message_kv.cpp`
- `examples/message_kv_demo.cpp`
- `tests/integration/test_message_kv.cpp`

Then run on both VMs:
```bash
cmake --build build -j4 --target test_message_kv message_kv_demo
```
Expected: both targets build on VM1; `message_kv_demo` builds on VM2.

- [ ] **Step 2: Run the new MessageKV tests on VM1**

Run:
```bash
./build/test_message_kv --gtest_filter="MessageKvIntegrationTest.AllocateSendRegionRequiresRunningNode:MessageKvIntegrationTest.AllocateSendRegionSucceedsAfterStart:MessageKvIntegrationTest.RecvCopiesSingleMessageIntoRegion:MessageKvIntegrationTest.SendUnpublishesMessageKeyAfterAck"
```
Expected: all pass.

- [ ] **Step 3: Run VM1/VM2 TCP smoke (`1K,1M`)**

VM1:
```bash
UCX_NET_DEVICES=enp0s1 ./build/message_kv_demo \
  --role rank0 \
  --listen 10.0.0.1:16030 \
  --data-addr 10.0.0.1:0 \
  --node-id rank0-receiver \
  --messages 4 \
  --warmup-rounds 1 \
  --sizes 1K,1M \
  --timeout-ms 30000 \
  --post-recv-wait-ms 4000 \
  --transport tcp
```

VM2:
```bash
UCX_NET_DEVICES=enp0s1 ./build/message_kv_demo \
  --role rank1 \
  --server-addr 10.0.0.1:16030 \
  --data-addr 10.0.0.2:0 \
  --node-id rank1-sender \
  --threads 4 \
  --warmup-rounds 1 \
  --sizes 1K,1M \
  --transport tcp
```
Expected: `SEND_ROUND` and `RECV_ROUND` both succeed for `1K` and `1M`.

- [ ] **Step 4: Run VM1/VM2 RDMA smoke (`1K,1M`)**

VM1:
```bash
UCX_NET_DEVICES=rxe0:1 ./build/message_kv_demo \
  --role rank0 \
  --listen 10.0.0.1:16040 \
  --data-addr 10.0.0.1:0 \
  --node-id rank0-receiver \
  --messages 4 \
  --warmup-rounds 1 \
  --sizes 1K,1M \
  --timeout-ms 30000 \
  --post-recv-wait-ms 4000 \
  --transport rdma
```

VM2:
```bash
UCX_NET_DEVICES=rxe0:1 ./build/message_kv_demo \
  --role rank1 \
  --server-addr 10.0.0.1:16040 \
  --data-addr 10.0.0.2:0 \
  --node-id rank1-sender \
  --threads 4 \
  --warmup-rounds 1 \
  --sizes 1K,1M \
  --transport rdma
```
Expected: no `remote access error`, both measured rounds complete.

- [ ] **Step 5: Record the measured `1MiB` sender-side result**

Capture the `SEND_ROUND round=1 size=1048576 ...` line from TCP and RDMA. Compare it against the previous broken/slow result to confirm the safe `send_region()` path is now usable under RDMA.

- [ ] **Step 6: Commit only if verification required cleanup edits**

```bash
git add include/zerokv/kv.h src/kv/node.cpp include/zerokv/message_kv.h src/message_kv.cpp examples/message_kv_demo.cpp tests/integration/test_message_kv.cpp
if ! git diff --cached --quiet; then
  git commit -m "Verify MessageKV send region context path"
fi
```
