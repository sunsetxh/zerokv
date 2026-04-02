# MessageKV Concurrent Fetch Design

Date: 2026-04-02
Status: Draft

## 1. Goal

Improve `MessageKV` large-payload throughput, especially for `1MiB+` messages, by allowing `recv_batch()` to keep multiple `fetch_to` operations in flight instead of converging each message mostly serially.

This is a performance-focused internal change:

- do not change the public `MessageKV` API
- do not change key naming or ack semantics
- do not change protocol or server behavior
- keep `send()` and `send_region()` both available

The target scenario remains:

- one receiver process
- one sender process with 4 sending threads
- 4 unique keys per round
- large payload sweeps such as `1K,64K,1M,4M,16M,32M,64M,100M`

## 2. Current Behavior

Today `MessageKV::recv_batch()`:

1. subscribes all keys
2. tries immediate `fetch_to` for each key
3. waits for any matching subscription event
4. when a key becomes ready, performs that key's `fetch_to`
5. publishes ack for that key

This removed the old whole-batch ack barrier, but it still leaves most of the receive-side data path effectively serialized:

- one key becomes ready
- one `fetch_to` is issued and completed
- then the loop moves on

That is acceptable for correctness and small messages, but it leaves throughput on the table for `1MiB+` payloads where multiple concurrent gets should better utilize the data path.

## 3. Options Considered

### Option A: Keep current wait-any plus serial `fetch_to`

Pros:

- already implemented
- simple and stable

Cons:

- likely leaves receive-side bandwidth underutilized for large payloads
- still trails `kv_bench --mode bench-fetch-to` by a meaningful margin in the large-size range

### Option B: Add multi-outstanding `fetch_to` inside `recv_batch()`

Pros:

- directly attacks the likely large-payload bottleneck
- preserves current public API and message semantics
- scales naturally with the existing 4-key-per-round demo and real scenario

Cons:

- more state to track per pending key
- requires careful completion bookkeeping and timeout handling

### Option C: First optimize control-plane overhead only

Pros:

- lower implementation risk

Cons:

- likely not the first-order bottleneck for `1MiB+`
- does not address receive-side serialization

## 4. Recommendation

Choose **Option B**.

For large payloads, the receiver should be able to:

- subscribe all pending keys
- notice readiness as events arrive
- issue `fetch_to` for multiple ready keys without waiting for earlier ones to finish
- ack each key as soon as its fetch completes

This keeps the existing correctness model while moving the receive-side behavior closer to a true multi-message data-plane path.

## 5. Design

### 5.1 Public API

No public API changes.

`MessageKV` remains:

- `send(key, data, size)`
- `send_region(key, region, size)`
- `recv(key, region, length, offset, timeout)`
- `recv_batch(items, region, timeout)`

`KVNode` keeps the existing `wait_for_any_subscription_event(...)` helper added in the previous round.

No new public API is introduced in this change.

### 5.2 Receiver State Machine

`MessageKV::recv_batch()` will be rewritten internally for the large-payload path.

The core flow:

1. validate layout as today
2. subscribe all unique keys
3. perform one immediate `fetch_to` attempt per key
4. keys that are not yet ready move to `pending`
5. maintain two sets:
   - `pending_ready`: keys ready to launch a fetch
   - `in_flight`: keys with a fetch already issued and not yet completed
6. while work remains and timeout has not expired:
   - use `wait_for_any_subscription_event(pending_keys, timeout_left)` to discover more ready keys
   - move newly ready keys to `pending_ready`
   - launch new fetches until `in_flight.size()` reaches a configured cap
   - poll or complete in-flight fetches
   - for each completed successful fetch:
     - mark key completed
     - publish ack immediately
     - remove from `in_flight`
   - for each failed fetch:
     - mark key failed
     - remove from `in_flight`
7. on timeout:
   - remaining non-completed keys become `timed_out`
8. unsubscribe subscribed keys
9. perform normal bounded ack cleanup

### 5.3 Outstanding Fetch Cap

Phase 1 of this optimization uses a small fixed cap on in-flight fetches:

- default: `4`

Rationale:

- matches the current scenario of 4 messages per round
- keeps implementation simple
- avoids turning this round into a generalized transport scheduler

This cap stays internal for now. No CLI knob and no public config knob in this round.

### 5.4 Fetch Completion Model

This design assumes `KVNode::fetch_to(...)` can be launched and then waited on independently via its existing future/result path.

The implementation should not add extra background threads. The entire receive-side state machine remains single-threaded inside `MessageKV::recv_batch()`.

That means:

- no one-thread-per-key waiting
- no detached worker threads
- no new executor

### 5.5 Ack Semantics

Ack semantics stay unchanged:

- receiver publishes ack for a key only after that key's data has been fetched successfully
- sender waits for that key's ack and then unpublishes the message key

The only change is that multiple keys may now reach the “fetched successfully” point with overlap, so acks can be emitted closer together and earlier relative to batch completion.

### 5.6 Trace Logging

Keep the existing env-var-gated trace switches:

- `ZEROKV_MESSAGE_KV_TRACE`
- `ZEROKV_KV_TRACE`

For this round, trace output should focus on:

- fetch launch
- fetch completion
- current `in_flight` count
- ack publish timing
- batch completion window

Trace remains debugging-only and is not part of the API contract.

## 6. Non-Goals

This round explicitly does **not** include:

- protocol changes
- server changes
- a new public concurrency knob
- sender-side semantic changes
- replacing `send()` with `send_region()` everywhere
- exposing async `recv_batch()` publicly
- generalized transport-level pipelining beyond this MessageKV path

## 7. Risks

### 7.1 No Actual Concurrency Gain

If `fetch_to(...)` internally blocks until completion and cannot overlap in practice, this change may add state complexity without meaningful throughput gains.

Mitigation:

- verify with VM and real hardware before treating this as finished
- keep the cap small and implementation contained

### 7.2 More Complex Timeout/Failure Bookkeeping

With multiple in-flight fetches, it is easier to get completion/failure/timed_out classification wrong.

Mitigation:

- preserve explicit per-key state
- add focused tests for mixed completion and timeout cases

### 7.3 Trace Noise

The new state machine could produce much noisier logs.

Mitigation:

- keep traces short and state-oriented
- avoid per-iteration spam when nothing changes

## 8. Testing

Required coverage:

1. existing `MessageKV` tests continue to pass unchanged
2. add a test that `recv_batch()` still acknowledges a completed key before a sibling key arrives
3. add a test that multiple large-ish keys can complete without regressing partial-timeout behavior
4. keep `wait_for_any_subscription_event(...)` tests passing
5. VM validation:
   - TCP dual-node: `1K,1M`
   - RDMA dual-node: `1K,1M`
6. performance spot-check:
   - compare before/after at `1M`, `16M`, `64M`
   - record sender `SEND_ROUND`
   - record receiver `RECV_ROUND`

## 9. Success Criteria

This round is considered successful if:

1. public `MessageKV` API remains unchanged
2. correctness tests stay green
3. dual-node TCP and RDMA validation still pass
4. large-payload receiver throughput improves measurably relative to the current baseline
5. no new correctness regressions appear in ack or timeout behavior
