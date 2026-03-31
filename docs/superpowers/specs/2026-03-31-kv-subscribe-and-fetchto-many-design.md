# KV Subscribe-And-Fetch-To-Many Design

## Goal

Add a batch helper that combines:

- waiting for keys that may not exist yet
- best-effort subscription-based readiness detection
- zero-copy placement into caller-provided registered memory

The target scenario is:

1. the caller prepares a shared output `MemoryRegion`
2. some keys already exist, some do not
3. AXON waits for missing keys to appear
4. as soon as an individual key becomes ready, AXON fetches it directly into
   the caller's region at the requested offset
5. the helper returns only after the whole batch is either completed or timed
   out

## Scope

In scope:

- Add a synchronous batch helper on `KVNode`
- Reuse the existing subscription, lookup, and `fetch_to()` flows
- Reuse the existing `FetchToItem` layout type
- Start zero-copy fetches per key as soon as each key becomes ready
- Return partial progress on timeout

Out of scope:

- New server protocol messages
- Callback-based APIs
- New wire protocol for fetch
- Automatic layout planning
- Owner-aware concurrency scheduling
- Batch metrics structs
- Re-fetching newer versions after first successful zero-copy fetch

## API

Add a new result type and method to `axon::kv`.

```cpp
struct BatchFetchToResult {
    std::vector<std::string> completed;
    std::vector<std::string> failed;
    std::vector<std::string> timed_out;
    bool completed_all = false;
};

BatchFetchToResult subscribe_and_fetch_to_once_many(
    const std::vector<FetchToItem>& items,
    const axon::MemoryRegion::Ptr& region,
    std::chrono::milliseconds timeout);
```

## Relationship to existing APIs

- `fetch_to_many(items, region)`
  - assumes keys already exist
  - no subscribe / no waiting
- `subscribe_and_fetch_once_many(keys, timeout)`
  - waits for keys and fetches owning bytes
- `subscribe_and_fetch_to_once_many(items, region, timeout)`
  - waits for keys and fetches zero-copy into caller memory

This new helper is the zero-copy companion to `subscribe_and_fetch_once_many`.

## Semantics

### Input layout

`FetchToItem` keeps the same meaning as `fetch_to_many`:

- `key`: key to fetch
- `length`: caller-provided output capacity for that key
- `offset`: byte offset into the shared output region

The layout validation rules are identical to `fetch_to_many`:

- non-empty item list
- non-empty keys
- non-zero lengths
- non-null region
- in-bounds `[offset, offset + length)`
- no overlapping ranges

Validation happens before any subscribe or fetch activity begins.

### Execution model

The helper is synchronous and blocks until:

- all keys are fetched successfully, or
- the timeout expires

The helper does **not** wait for all keys to become ready before starting
fetches.

Instead:

1. validate layout
2. deduplicate by key while preserving the original item list
3. for each unique key:
   - if the key already exists, attempt `fetch_to()` immediately
   - otherwise subscribe to the key and mark it pending
4. while pending keys remain and timeout has not expired:
   - drain subscription events
   - for matching `published` / `updated` events, try `fetch_to()` immediately
   - periodically retry lookup on pending keys
5. cleanup helper-created subscriptions
6. return aggregate result

### First-success-wins

Per key, the first successful `fetch_to()` wins.

If a key is fetched successfully once:

- it is removed from the pending set
- later events for the same key are ignored

This matches the existing `subscribe_and_fetch_once_many` rule.

## Duplicate key behavior

The input item list may contain the same key more than once at different
offsets.

Phase 1 should support that by treating the batch as:

- a set of unique readiness states by key
- a list of output placements per key

When a key becomes ready, the helper should perform zero-copy fetches for every
item mapped to that key.

Success/failure reporting is still per input item key name in this phase:

- every successful placement appends that key to `completed`
- every failed placement appends that key to `failed`

This means duplicate keys may appear multiple times in `completed` or `failed`,
matching the caller's item list semantics rather than unique-key semantics.

## Result semantics

```cpp
struct BatchFetchToResult {
    std::vector<std::string> completed;
    std::vector<std::string> failed;
    std::vector<std::string> timed_out;
    bool completed_all = false;
};
```

- `completed`:
  - every input item whose zero-copy placement succeeded
- `failed`:
  - every input item that reached a terminal fetch failure
- `timed_out`:
  - every unique key still pending when timeout expires
- `completed_all`:
  - true only if every input item succeeded

`timed_out` is keyed by logical key availability, not by placement count.

## Event rules

- `published` and `updated` trigger immediate zero-copy fetch attempts for
  still-pending keys
- `unpublished` and `owner_lost` do not count as success
- if a fetch attempt fails with a retryable "still missing" style error, the
  key remains pending
- if a fetch attempt fails with a terminal error, all placements for that key
  move to `failed`

## Cleanup rules

As with `wait_for_keys` and `subscribe_and_fetch_once_many`:

- track which subscriptions were created by this helper
- cleanup only unsubscribes keys from that set
- pre-existing user subscriptions must be preserved

## Retry / consistency model

Subscription delivery remains best-effort, so the helper must not rely on
events alone.

The helper should reuse the same consistency pattern as the existing wait/fetch
helpers:

1. initial lookup
2. subscription event polling
3. periodic lookup fallback on still-pending keys

Phase 1 should reuse the existing fixed retry cadence of roughly 100 ms.

## Tests

Phase 1 should cover at least:

1. keys already exist, immediate zero-copy success
2. key missing at first, later published, then zero-copy success
3. batch timeout returns partial progress
4. duplicate key writes to multiple offsets after one readiness event
5. helper preserves pre-existing subscriptions
6. invalid layout fails before subscribe activity

## Notes

- This design builds directly on top of `fetch_to_many` and the existing
  wait-and-fetch helpers.
- It keeps the wire protocol unchanged.
- It is intentionally synchronous in Phase 1.
