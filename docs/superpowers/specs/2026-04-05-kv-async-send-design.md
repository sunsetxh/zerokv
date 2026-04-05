# KV Async Send Design

## Overview

This design adds an asynchronous sender path to `zerokv::KV` for large-payload
steady-state workloads.

Today:

- `KV::send()` and `KV::send_region()` are synchronous
- they publish the message
- then block waiting for the receiver ack
- then unpublish the message key before returning

That behavior is simple and correct, but it forces sender-side throughput to
include:

- ack propagation latency
- metadata cleanup latency
- any control-path jitter in the ack/unpublish sequence

For `1MiB+` payloads, the receiver side is already close to the underlying
`fetch_to` data-path capability, while the sender side remains visibly lower
because it waits for the full ack+cleanup lifecycle.

The goal of this design is to preserve the existing synchronous API while
introducing a new async sender API that returns immediately after publish and
completes later when cleanup is finished.

## Goals

- Keep existing `KV::send()` and `KV::send_region()` semantics unchanged
- Add async sender APIs that return `zerokv::transport::Future<void>`
- Define async completion as:
  - receiver ack observed
  - sender `unpublish(message_key)` succeeded
  - message metadata cleanup completed successfully
- Allow multiple in-flight sends to share one internal cleanup thread per `KV`
  instance
- Improve sender steady-state throughput for large-payload workloads

## Non-Goals

- No change to `recv()` / `recv_batch()` semantics
- No change to wire protocol
- No attempt to make sender completion fire-and-forget
- No batch sender queue API in this phase
- No multi-threaded cleanup pool; one cleanup thread per `KV` instance is enough

## Proposed API

Add two new methods on `zerokv::KV`:

```cpp
zerokv::transport::Future<void> send_async(const std::string& key,
                                           const void* data,
                                           size_t size);

zerokv::transport::Future<void> send_region_async(
    const std::string& key,
    const zerokv::transport::MemoryRegion::Ptr& region,
    size_t size);
```

### Completion contract

A returned future becomes ready only when:

1. the receiver ack key was observed
2. the sender successfully unpublished the original message key
3. any internal cleanup bookkeeping for that send has completed

If ack wait fails, unsubscribe fails fatally, or unpublish fails, the future
completes with error.

### Existing synchronous APIs

`send()` and `send_region()` remain public and unchanged in meaning.
Implementation may internally delegate to the async path and then block on the
returned future.

## Internal Model

### Pending send state

Each async send registers one pending item in `KV::Impl`, containing:

- `message_key`
- `ack_key`
- the promise backing the returned `Future<void>`
- state flags such as `subscribed`, `acked`, `completed`
- timestamps for optional trace logging

### Cleanup thread

Each started `KV` instance owns one internal cleanup thread.

The thread waits on a condition variable and processes pending sends by:

1. subscribing to ack keys if needed
2. waiting for matching ack events
3. falling back to existence check only when necessary
4. unsubscribing the ack key
5. unpublishing the message key
6. fulfilling or failing the promise

This thread is the only async cleanup executor for that `KV` instance.

### Why one thread is acceptable

- async sends are sender-side control work, not bulk data movement
- the heavy data path remains receiver-side `fetch_to`
- one cleanup thread avoids thread explosion and keeps ordering easy to reason
  about

## Data Flow

### `send_region_async()`

1. validate key/region/running state
2. publish the message key immediately
3. create promise/future pair
4. enqueue pending send record
5. notify cleanup thread
6. return future to caller

### cleanup thread loop

For each pending send:

1. ensure ack subscription is active
2. wait for `ack_key` publication event
3. if event path misses, do a final existence check
4. unsubscribe `ack_key`
5. unpublish `message_key`
6. set promise success or failure
7. remove pending send from active set

### `send()` / `send_region()`

These may be reimplemented as:

1. call async variant
2. wait on returned future
3. propagate status via existing exception path

This preserves public semantics while removing duplicate lifecycle logic.

## Error Handling

### Immediate errors

The async methods throw immediately for the same input/running-state errors as
current sync methods:

- empty key -> `kInvalidArgument`
- null data / null region -> `kInvalidArgument`
- oversize send_region -> `kInvalidArgument`
- node not running -> `kConnectionRefused`
- publish failure -> throw immediately

### Deferred errors

After publish succeeds, later ack/cleanup failures are reported through the
returned future:

- ack wait timeout -> future error
- subscription failure after enqueue -> future error
- unsubscribe failure that prevents cleanup completion -> future error
- unpublish failure -> future error

## Stop / Shutdown Semantics

`KV::stop()` must remain bounded.

Proposed behavior:

- signal cleanup thread to stop accepting new work
- drain any pending sends for up to a bounded time budget
- pending futures that cannot be completed in time fail with
  `kConnectionReset` or `kTimeout`
- join cleanup thread before releasing `KVNode`

This keeps shutdown deterministic while avoiding silent metadata leaks for
in-flight async sends.

## Tracing

Keep `ZEROKV_MESSAGE_KV_TRACE` and add async-specific lines such as:

- `MESSAGE_KV_SEND_ASYNC_ENQUEUE`
- `MESSAGE_KV_ASYNC_ACK_WAIT`
- `MESSAGE_KV_ASYNC_CLEANUP`
- `MESSAGE_KV_SEND_ASYNC_COMPLETE`

Trace format is diagnostic only and not API contract.

## Testing

1. API surface test
- `KV` exposes `send_async()` and `send_region_async()` returning
  `transport::Future<void>`

2. async completion test
- sender calls `send_region_async()`
- receiver `recv()`s message and publishes ack
- returned future becomes ready and success
- original message key is eventually unpublished

3. async cleanup failure test
- simulate receiver never acking
- future completes with timeout/error

4. sync compatibility test
- existing `send()` / `send_region()` tests still pass unchanged

5. shutdown test
- enqueue async send
- stop sender before ack arrives
- future resolves with failure rather than hanging forever

## Expected Impact

This change should not materially alter receiver throughput.

It should improve sender-side steady-state throughput because publish returns
immediately and ack/unpublish move off the application thread.

The main expected gains are on:

- `1MiB+` large-payload sender throughput
- workloads with multiple sender threads issuing independent sends
- cases where receiver is already fast but sender is blocked on lifecycle
  cleanup
