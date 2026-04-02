# MessageKV Send Region Context Design

## Goal

Fix the RDMA correctness gap exposed by `message_kv_demo` after switching the
sender path to `send_region()`.

The immediate issue is that the demo currently allocates sender `MemoryRegion`
objects from an external `Context`, while `MessageKV` internally publishes those
regions through its own `KVNode`, which owns a different `Context`. This works
for some TCP paths but fails for RDMA with remote access errors.

## In Scope

- Add a public `MessageKV` helper for allocating send regions from the same
  transport context that backs the internal `KVNode`
- Update `message_kv_demo` to use that helper instead of creating external
  `Context` objects per sender thread
- Re-run TCP/RDMA demo validation on VM1/VM2

## Out of Scope

- Removing `send()` or `send_region()`
- Adding background cleanup or changing ack semantics
- Adding a generic user-buffer registration API in this change
- Changing receiver behavior

## Design

### Problem Statement

`MemoryRegion` registration is bound to a specific `Context`. `MessageKV`
internally owns a `KVNode`, and `KVNode` internally owns its own `Context`.

The current demo sender path does this:

1. create external `Context`
2. allocate `MemoryRegion` from that external `Context`
3. pass the region into `MessageKV::send_region()`
4. `MessageKV` forwards it to `KVNode::publish_region()`

Under RDMA, this mixes region ownership and publishing ownership across
different contexts and can produce remote access failures.

### Public API Addition

Add:

```cpp
[[nodiscard]] zerokv::MemoryRegion::Ptr allocate_send_region(size_t size);
```

to `MessageKV`.

Semantics:

- valid only after `start()`
- allocates a registered region using the same internal context that backs the
  `KVNode`
- returns a region suitable for subsequent `send_region()` calls on the same
  `MessageKV` instance
- throws `kConnectionRefused` if called before `start()`
- throws `kRegistrationFailed` if allocation fails

This is the minimal safe performance-path helper.

### Implementation Shape

`MessageKV::allocate_send_region(size)` should:

1. lock the wrapper mutex
2. confirm the node is running
3. delegate to a small `KVNode` allocation helper that uses the node's internal
   context
4. return the region

Minimal `KVNode` addition:

```cpp
[[nodiscard]] zerokv::MemoryRegion::Ptr allocate_region(size_t size) const;
```

Semantics:

- allocate via `MemoryRegion::allocate(impl_->context_, size)`
- return `nullptr` if allocation fails

This helper is intentionally narrow: it exposes allocation, not arbitrary
context access.

### Demo Change

`message_kv_demo` sender threads should stop creating external `Context`
instances. Instead, each thread should:

1. create and start its own `MessageKV`
2. call `mq->allocate_send_region(max_size_bytes)`
3. reuse that region across warmup and measured rounds
4. copy payload bytes into the region front
5. call `send_region()`

This preserves the current sender threading model while ensuring region/context
compatibility under RDMA.

### Future Work

If users already own raw buffers and want zero-copy send from them, add a later
API such as:

```cpp
[[nodiscard]] zerokv::MemoryRegion::Ptr register_send_buffer(
    void* addr, size_t length, MemoryType type = MemoryType::kHost);
```

That future API must also register memory against the internal `KVNode`
context.

## Testing

1. Unit/integration:
   - new `MessageKV` test for `allocate_send_region()` after `start()`
   - new `MessageKV` test for `allocate_send_region()` before `start()`
2. Demo helper tests still pass
3. VM1/VM2 TCP:
   - `1K,1M` sweep passes
4. VM1/VM2 RDMA (`rxe0:1`):
   - `1K,1M` sweep passes
   - no `remote access error`

## Risks

- This introduces one more small public helper on `MessageKV`, but it is the
  narrowest API that makes `send_region()` safe for the demo and future
  performance-sensitive callers.
- This does not yet solve the “user already has a raw buffer” case; that is
  intentionally deferred to a later registration API.
