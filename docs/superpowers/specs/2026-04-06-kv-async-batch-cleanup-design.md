# KV Async Batch Cleanup Design

## Overview

`KV::send_async()` and `KV::send_region_async()` already let sender threads return
early, but the internal cleanup thread still processes pending sends one by one:

1. wait for one ack
2. unsubscribe that ack key
3. unpublish that message key
4. fulfill one future

That keeps the API correct, but it serializes sender-side completion behind the
cleanup thread's queue order. When many async sends are in flight, later sends
can wait behind unrelated earlier sends even if their acks arrived first.

This design keeps the current public async API and single-threaded cleanup
model, but changes cleanup from queue-serial ack wait to batch wait-any
processing.

## Goals

- Keep `KV::send_async()` / `KV::send_region_async()` public API unchanged
- Keep `send()` / `send_region()` semantics unchanged
- Keep exactly one cleanup thread per `KV` instance
- Allow cleanup to complete async sends in ack-arrival order rather than enqueue
  order
- Reduce sender-side async completion latency under multiple in-flight sends

## Non-Goals

- No recv-side changes
- No wire protocol changes
- No cleanup thread pool
- No new user-visible batch sender API
- No change to async completion contract

## Current Problem

Today each async send is enqueued as one `PendingAsyncSend`. The cleanup thread:

- pops the front item
- waits for only that item's ack key
- completes it before looking at the next item

This means the cleanup thread behaves like a FIFO barrier. If four async sends
are pending and ack 4 arrives first, send 4 still cannot complete until sends
1-3 have each been processed.

## Proposed Change

Keep one cleanup thread, but change its internal loop to manage a batch of
pending sends.

### New cleanup model

The cleanup thread maintains:

- `pending_by_ack_key`
- `ready_ack_keys`
- optional `ready_without_event` fallback results

Loop shape:

1. move any newly enqueued sends from the producer queue into the active pending
   map
2. if the active map is empty, wait on `cleanup_cv`
3. otherwise call `wait_for_any_subscription_event(active_ack_keys, slice)`
4. if one ack event arrives:
   - mark that ack key ready
5. also periodically run a cheap fallback existence check for active ack keys
   that may have missed an event
6. for every ready ack key:
   - unsubscribe ack key
   - unpublish message key
   - fulfill or fail its future
   - remove it from the active map

This preserves one-thread simplicity while letting completion follow actual ack
arrival order.

## Data Structures

Replace the cleanup thread's implicit FIFO-only work model with two structures:

- producer queue:
  - receives newly published async sends from `send_async()`
- active pending map:
  - keyed by `ack_key`
  - stores:
    - `message_key`
    - `ack_key`
    - `Promise<void>`
    - subscription bookkeeping
    - optional timestamps for trace logging

The producer queue remains useful so the send path can stay cheap. The cleanup
thread drains that queue into the active map whenever it wakes.

## Ack Detection

Primary path:

- `wait_for_any_subscription_event(active_ack_keys, timeout_slice)`

Fallback path:

- `wait_for_key(ack_key, 1ms)` only for keys still active after the event wait
  slice

The fallback remains important for correctness if an ack was published before a
subscription event was observed or if an event is dropped.

## Completion Contract

No semantic change:

A future becomes ready only when:

1. ack is observed
2. `unsubscribe(ack_key)` has been attempted
3. `unpublish(message_key)` succeeded

Failures still surface through the future status.

## Stop Semantics

No semantic change:

- `stop()` signals cleanup thread shutdown
- cleanup thread exits promptly
- remaining active or queued async sends fail with `kConnectionReset`
- node shutdown still happens after cleanup thread join

The only internal difference is that `stop()` now has to fail both:

- producer-queue items not yet activated
- active-map items already being tracked by ack key

## Tracing

Keep `ZEROKV_MESSAGE_KV_TRACE`, but add batch-cleanup-oriented lines:

- `MESSAGE_KV_ASYNC_BATCH_DRAIN`
- `MESSAGE_KV_ASYNC_BATCH_WAIT_ANY`
- `MESSAGE_KV_ASYNC_BATCH_READY`
- `MESSAGE_KV_ASYNC_BATCH_COMPLETE`

Trace stays diagnostic only.

## Testing

1. Completion-order test
- enqueue multiple async sends
- deliver receiver acks out of send order
- assert futures complete according to ack order, not enqueue order

2. Multi-inflight success test
- several `send_region_async()` calls
- receiver consumes all
- all futures complete success
- all message keys are unpublished

3. Stop-with-active-batch test
- enqueue multiple async sends
- stop before all acks arrive
- all incomplete futures resolve with failure

4. Sync regression test
- existing `send()` / `send_region()` tests still pass unchanged

5. VM validation
- compare `--send-mode async` sender-side `send_us` and round completion under
  multiple in-flight sends on TCP and RDMA
