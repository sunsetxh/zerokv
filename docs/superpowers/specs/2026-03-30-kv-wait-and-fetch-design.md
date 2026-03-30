# KV Wait And Fetch Design

## Goal

Add high-level client-side helpers for the common workflow where a node needs to
wait for keys that do not exist yet, then fetch them once they become
available.

The immediate target scenario is:

1. `node1` subscribes to `key1` before it exists.
2. `node2` later publishes or pushes `key1`.
3. `node1` waits for `key1` to become available.
4. `node1` fetches `key1`.

This design also covers the multi-key case, where the caller wants to wait for a
set of keys and fetch each key as soon as it becomes ready, while returning only
after the whole batch has either completed or timed out.

## Scope

In scope:

- Add synchronous wait helpers on `KVNode`
- Add single-key and batch "subscribe and fetch once" helpers on `KVNode`
- Reuse the existing subscription, lookup, and fetch flows
- Return partial results on batch timeout
- First-success-wins semantics for each key

Out of scope:

- New server protocol messages
- Callback-based APIs
- Streamed result delivery
- Strong event ordering or delivery guarantees beyond the current best-effort
  subscription model
- Re-fetching newer versions after the first successful fetch

## API

Add the following public types and methods to `axon::kv::KVNode`.

```cpp
struct WaitKeysResult {
    std::vector<std::string> ready;
    std::vector<std::string> timed_out;
    bool completed = false;
};

struct BatchFetchResult {
    std::vector<std::pair<std::string, FetchResult>> fetched;
    std::vector<std::string> missing;
    std::vector<std::string> timed_out;
    bool completed = false;
};

Status wait_for_key(const std::string& key,
                    std::chrono::milliseconds timeout);

WaitKeysResult wait_for_keys(const std::vector<std::string>& keys,
                             std::chrono::milliseconds timeout);

Future<FetchResult> subscribe_and_fetch_once(const std::string& key,
                                             std::chrono::milliseconds timeout);

Future<BatchFetchResult> subscribe_and_fetch_once_many(
    const std::vector<std::string>& keys,
    std::chrono::milliseconds timeout);
```

## Semantics

### `wait_for_key`

- If the key already exists, return success immediately.
- Otherwise, subscribe to the key and wait until it becomes available.
- Availability means that a fresh lookup succeeds.
- Timeout returns `Status(ErrorCode::kTimeout, ...)`.

### `wait_for_keys`

- Deduplicate the input keys.
- Keys that already exist are marked ready immediately.
- Missing keys are subscribed and waited on.
- The method returns when either:
  - all keys are ready, or
  - the timeout expires.
- Result semantics:
  - `ready`: keys confirmed available before timeout
  - `timed_out`: keys still unavailable when timeout expires
  - `completed`: true only if every key became available

### `subscribe_and_fetch_once`

- Single-key convenience wrapper.
- If the key already exists, fetch it immediately.
- Otherwise, subscribe and wait.
- On the first successful fetch, return that result and stop.
- Later updates to the same key are ignored.

### `subscribe_and_fetch_once_many`

This is the primary high-performance batch helper.

- Deduplicate the input keys.
- For keys that already exist, try to fetch them immediately.
- For keys that do not exist yet, subscribe and wait.
- When a `published` or `updated` event arrives for a pending key, attempt an
  immediate fetch for that key.
- As soon as a fetch succeeds for a key, that key is marked complete and is not
  re-fetched again.
- The method returns when either:
  - every key has been fetched successfully, or
  - the timeout expires.

This means the implementation does **not** wait for the whole batch to become
ready before starting fetches. Keys are fetched independently as soon as they
become ready.

### Timeout and partial results

Batch timeout returns partial results.

- `fetched`: keys that were fetched successfully before timeout
- `missing`: keys that never became available or never fetched successfully
- `timed_out`: keys still pending when timeout fired
- `completed`: true only if every key fetched successfully

For Phase 1, `missing` and `timed_out` may overlap semantically, but they are
kept separate because the caller often wants to distinguish:

- keys that never appeared, vs.
- keys that appeared but did not finish before timeout

The implementation should classify as follows:

- `timed_out`: keys still in the active pending set when timeout expires
- `missing`: keys that never produced a successful fetch by the end of the
  operation

## Event handling rules

- `published` and `updated` trigger a fetch attempt for the matching key if it
  is still pending.
- `unpublished` and `owner_lost` do not count as success.
- If a key becomes unavailable again after an event, the fetch attempt simply
  fails and the key remains pending until timeout or a later success.

## Consistency model

The implementation uses a best-effort event stream plus explicit lookup/fetch.
It must not rely on subscription delivery alone.

To avoid missing keys because of event loss or timing gaps:

1. perform an initial lookup pass before subscribing
2. after subscribing, poll subscription events
3. when events are sparse, periodically retry lookup on still-pending keys

This keeps the helpers robust even if an event is missed.

## Internal implementation

All logic stays in `KVNode`. No server changes are needed.

Suggested internal flow for batch fetch:

1. Deduplicate keys
2. For each key:
   - if lookup succeeds, try fetch immediately
   - otherwise add to `pending_lookup`
3. Subscribe to all still-pending keys
4. Until success or timeout:
   - drain subscription events
   - for matching `published/updated` events, try fetch immediately
   - periodically re-check pending keys with lookup
5. Unsubscribe from any keys that were subscribed by this operation
6. Return aggregate result

## First-success-wins rule

For Phase 1, each key is locked on the first successful fetch.

If a key receives:

- `published(version=1)`
- then `updated(version=2)`

and version 1 fetch already succeeded, the operation keeps version 1 and does
not refetch. This keeps the state machine simple and matches the `once`
semantics.

## Error handling

- Empty key is invalid
- Empty batch is invalid
- Duplicate keys are deduplicated, not treated as an error
- Subscribe failures abort the operation immediately
- Individual fetch failures do not abort the whole batch; the key remains
  pending unless the operation times out
- Unsubscribe failures during cleanup are best-effort and should not override a
  more important timeout or fetch error

## Performance expectations

The batch helper is intended for high-performance multi-key workflows.

Compared with a naive implementation that waits for all keys and only then
starts fetching, this design reduces tail latency because:

- each key can start fetching as soon as it becomes ready
- slower keys do not delay already-ready keys from being fetched

This does not change the underlying server or RDMA data path. It only improves
client-side orchestration.

## Testing

Add coverage for at least these cases:

1. single key: subscribe before publish, then fetch succeeds
2. single key: key already exists, immediate fetch succeeds without waiting
3. batch: multiple keys published at different times, each fetched when ready,
   final result completes successfully
4. batch timeout: partial success plus timed-out keys
5. duplicate keys in input are deduplicated
6. repeated `updated` events after first successful fetch do not trigger a
   second fetch

## Non-goals for this phase

- bulk RDMA fetch optimization across multiple keys
- speculative prefetch
- latest-version tracking
- persistent watchers that continuously stream new values
- server-side wait queues
