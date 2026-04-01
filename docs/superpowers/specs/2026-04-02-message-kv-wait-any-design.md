# MessageKV Wait-Any Receive Path Design

## Summary

This change introduces a blocking `wait_for_any_subscription_event(...)` helper in
`KVNode` and rewrites `MessageKV::recv_batch()` to use a single-threaded
subscribe-all / wait-any / fetch-to / ack state machine.

The goal is to reduce the current batch receive barrier in the first MessageKV
scenario:

- `RANK1` sends 4 unique keys concurrently
- `RANK0` receives the 4 keys as one logical batch
- sender-side `send()` should not wait for unrelated keys longer than necessary
- receiver-side collection should react to whichever key becomes ready first

The implementation keeps the public `MessageKV` API unchanged, adds no background
threads, and preserves the current protocol.

## Motivation

The current experimental evidence shows two things:

1. demo-only warmup and cross-round reuse fix most cold-start inflation
2. the library still benefits from replacing batch-level receive barriers with
   per-key completion and per-key ack publication

The existing `MessageKV::recv_batch()` path delegates to a helper that only
returns once the batch reaches a final state. That structure delays per-key ack
publication and makes sender-side completion depend more on batch convergence than
necessary.

This design promotes the already-validated experimental direction into a clean,
reviewable library change.

## Goals

- add `KVNode::wait_for_any_subscription_event(keys, timeout)`
- let `MessageKV::recv_batch()` fetch and ack keys one by one as they become ready
- keep the implementation single-threaded
- keep public `MessageKV` method signatures unchanged
- retain traceability through environment-variable-gated debug logs
- preserve existing ownership rules:
  - sender owns message keys
  - receiver owns ack keys

## Non-Goals

- no protocol changes
- no background cleanup threads
- no async `fetch_to`
- no changes to the demo warmup behavior in this task
- no attempt to optimize beyond a single-threaded event-driven batch state machine
- no wire-format or server-side subscription redesign

## Public API Changes

### KVNode

Add:

```cpp
std::optional<SubscriptionEvent> wait_for_any_subscription_event(
    const std::vector<std::string>& keys,
    std::chrono::milliseconds timeout);
```

Semantics:

- waits for a queued subscription event matching any key in `keys`
- returns the first matching event observed within `timeout`
- returns `std::nullopt` on timeout
- ignores unrelated queued events but preserves them for later consumers
- requires the node to be running
- empty `keys` is invalid input

No public `MessageKV` API shape changes are required.

## Design

### 1. `KVNode::wait_for_any_subscription_event`

Implementation constraints:

- reuse the existing subscription event queue and condition variable
- do not introduce a new subscription transport or thread
- preserve unrelated events in the queue

Behavior:

1. validate `running == true`
2. validate `keys` is non-empty
3. deduplicate the input key list into a lookup set
4. under the subscription event lock:
   - scan the current queue for the first matching event
   - if found:
     - remove and return it
   - if not found:
     - wait on the condition variable until timeout or new events arrive
5. on wake-up, rescan the queue
6. if timeout expires without a matching event, return `std::nullopt`

Only `published` and `updated` events are useful to the current MessageKV receive
path, but the helper returns the full `SubscriptionEvent` so future callers can
interpret the type themselves.

### 2. `MessageKV::recv_batch()` State Machine

`recv_batch()` will no longer delegate to
`KVNode::subscribe_and_fetch_to_once_many(...)`.

Instead it will:

1. validate batch layout locally
2. build `placements_by_key`
3. subscribe all unique keys
4. immediately attempt `fetch_to` for each key once
5. for remaining pending keys:
   - call `wait_for_any_subscription_event(pending_keys, timeout_left)`
   - when a key becomes ready, attempt `fetch_to` for that key
   - on success:
     - append all placements for that key to `completed`
     - publish that key's ack immediately
     - record the ack key for later receiver-owned cleanup
   - on terminal failure:
     - append placements to `failed`
   - on retryable absence:
     - leave the key pending
6. on timeout:
   - append remaining unique keys to `timed_out`
7. unsubscribe all keys that were subscribed by the helper

This keeps the batch logic single-threaded while removing the old "entire batch
must finish before ack publication" behavior.

### 3. Ack Semantics

Ack ownership remains unchanged:

- receiver publishes ack keys
- sender waits for its ack key, then unpublishes its message key
- receiver later cleans up its own ack keys

What changes is timing:

- previously, `recv_batch()` could effectively delay all acks until batch exit
- after this change, a key is acked immediately after its successful fetch

### 4. Validation

`recv_batch()` should continue to reject invalid layouts before any waiting:

- null region
- empty items
- empty key
- zero length
- out-of-bounds range
- overlapping ranges

This validation can remain local to `MessageKV` for now.

### 5. Trace Logging

Keep environment-variable-gated trace hooks:

- `ZEROKV_KV_TRACE`
- `ZEROKV_MESSAGE_KV_TRACE`

But the logs should stay focused on state transitions needed for performance
analysis:

For `KVNode`:

- wait-any start
- wait-any match
- wait-any timeout

For `MessageKV`:

- recv-batch start
- immediate fetch success/failure
- wait-any wake-up event key
- per-key fetch begin / done
- per-key ack publish
- recv-batch final summary

Trace output remains a debugging aid, not part of the API contract.

## Error Handling

### `wait_for_any_subscription_event`

- node not running:
  - throw `kConnectionRefused`
- empty key list:
  - throw `kInvalidArgument`
- timeout:
  - return `std::nullopt`

### `recv_batch()`

- layout validation errors:
  - throw immediately
- per-key successful fetch:
  - key enters `completed`
- per-key terminal fetch error:
  - key enters `failed`
- timeout with pending keys:
  - keys enter `timed_out`

### `recv()`

No semantic change:

- still wraps the batch path for one item
- still throws on timeout or failure

## Testing

### Unit / Integration

1. `KVNode::wait_for_any_subscription_event` returns when one of multiple keys
   becomes ready
2. `KVNode::wait_for_any_subscription_event` times out when no matching key
   arrives
3. `MessageKV::recv_batch()` acknowledges a completed key before a slower sibling
   key completes
4. existing `recv_batch()` partial-timeout behavior remains correct
5. existing send/recv/unpublish-after-ack behavior remains correct

### VM Validation

Run on VM1/VM2:

- TCP:
  - `UCX_NET_DEVICES=enp0s1`
  - `--sizes 1K`
- RDMA:
  - `UCX_NET_DEVICES=rxe0:1`
  - `--sizes 1K`

Compare:

- sender `send_us`
- receiver completion window from trace

Expected qualitative outcome:

- sender `send_us` should improve relative to the old batch barrier path
- receiver completion should proceed per key, not as a single final barrier

## Risks

### Event Queue Interaction

`wait_for_any_subscription_event(...)` must preserve unrelated queued events.
Dropping unrelated events would break other subscription users.

### Retryable vs Terminal Fetch Failures

The state machine must keep absent-but-not-yet-published keys pending rather than
misclassifying them as permanent failures.

### Trace Noise

Leaving too much instrumentation in the fast path can distort the very timings we
are trying to observe. Keep the trace concise and gated.

## Future Work

- async `fetch_to` with multiple outstanding gets
- a richer blocking event API (`wait_for_events(predicate, timeout)`)
- further shrink receiver completion window after the batch barrier is removed
