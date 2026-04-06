# KV Lazy Ack Unsubscribe Design

## Overview

`KV::send_async()` and `KV::send_region_async()` now complete in ack-arrival
order, but each completion still performs two sender-side control-plane actions
on the hot path:

1. `unsubscribe(ack_key)`
2. `unpublish(message_key)`

The second step is required for correctness because the message metadata must be
removed before the future can complete. The first step is housekeeping: it
removes the sender's subscription to the receiver-owned ack key.

This design removes `unsubscribe(ack_key)` from the sender completion hot path
and turns it into lazy cleanup work handled later by sweep/stop. The future
completion contract becomes:

- ack observed
- message key successfully unpublished

The sender may still remain subscribed to the ack key temporarily until a later
cleanup pass.

## Goals

- Reduce sender hot-path control-plane work for both sync and async sends
- Keep `send_async()` / `send_region_async()` public API unchanged
- Keep `send()` / `send_region()` semantics unchanged
- Preserve guaranteed message metadata cleanup before future completion
- Keep unsubscribe cleanup bounded and deterministic at `stop()`

## Non-Goals

- No recv-side changes
- No wire protocol changes
- No subscription protocol redesign
- No batch unsubscribe API in this phase
- No change to message-key ownership or ack-key ownership

## Current Problem

Today async cleanup completes a ready send by:

1. `unsubscribe(ack_key)`
2. `unpublish(message_key)`
3. fulfill promise

That makes sender completion pay for an extra control RPC that does not affect
message visibility or correctness of the completed send.

Since sync `send()` delegates to async + wait, this same unsubscribe cost is
also paid on the synchronous path.

## Proposed Change

Move `unsubscribe(ack_key)` out of the async completion hot path.

### New completion sequence

When sender cleanup observes `ack_key`:

1. mark the ack subscription as stale
2. `unpublish(message_key)`
3. fulfill or fail the future
4. leave `ack_key` subscription cleanup for later sweep

### Deferred cleanup

`KV::Impl` keeps a list/set of stale sender ack subscriptions that still need
`unsubscribe`.

Those stale subscriptions are cleaned up:

- opportunistically at the start of future send operations
- during `KV::stop()`

This mirrors how receiver-owned ack keys are already cleaned up lazily.

## Data Structures

Extend `KV::Impl` with sender-side deferred unsubscribe tracking, for example:

- `std::vector<std::string> stale_sender_ack_keys`
  or
- `std::unordered_set<std::string> stale_sender_ack_keys`

Requirements:

- avoid duplicate unsubscribe work if possible
- keep stop-path draining simple

## Completion Contract

### Async

No API shape change, but the precise internal completion condition becomes:

1. ack observed
2. `unpublish(message_key)` succeeded

`unsubscribe(ack_key)` is no longer required before fulfilling the future.

### Sync

No user-visible semantic change:

- `send()` / `send_region()` still return only after ack has been observed and
  message metadata has been deleted

## Cleanup Semantics

### Opportunistic sweep

At existing sweep points:

- before new sends
- before/after recv paths where sweep already runs

also attempt sender-side `unsubscribe(ack_key)` cleanup for stale ack
subscriptions.

### Stop

`KV::stop()` must still guarantee bounded cleanup:

- signal cleanup thread
- join cleanup thread
- unsubscribe all stale sender ack keys best-effort
- fail remaining queued/active futures
- stop node

Any unsubscribe failures remain non-fatal once the corresponding message key has
already been unpublished and the user future has completed.

## Correctness Notes

- Receiver still owns the ack key object itself; sender only removes its
  subscription, not the ack key.
- Leaving a temporary sender subscription active is acceptable as long as:
  - it is eventually unsubscribed
  - stale ack events do not complete unrelated sends

To keep this safe, pending-send state must still key completion by exact
`ack_key`.

## Tracing

Keep `ZEROKV_MESSAGE_KV_TRACE` and add lazy-cleanup-specific lines:

- `MESSAGE_KV_ASYNC_DEFER_UNSUBSCRIBE`
- `MESSAGE_KV_SWEEP_ACK_UNSUBSCRIBE`
- `MESSAGE_KV_SWEEP_ACK_UNSUBSCRIBE_FAILED`

Trace is diagnostic only.

## Testing

1. Async completion regression
- `send_region_async()` still completes successfully after receiver ack
- message key is unpublished before future completion

2. Sync completion regression
- `send_region()` still blocks until message key is unpublished

3. Deferred unsubscribe cleanup test
- complete an async send
- verify a later sweep/stop removes the stale sender ack subscription without
  affecting completed-send correctness

4. Multiple-send regression
- batch async cleanup tests still pass unchanged

5. VM validation
- compare sync/async sender metrics before vs after removing hot-path
  `unsubscribe(ack_key)`
