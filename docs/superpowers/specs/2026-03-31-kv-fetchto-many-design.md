# KV Fetch-To-Many Design

## Goal

Add a batch zero-copy fetch API that writes multiple keys into caller-provided
registered memory without allocating per-key result buffers.

The immediate target is a high-throughput path where the application already
owns a large `MemoryRegion` and wants AXON to place multiple fetched values into
that region using explicit offsets.

## Scope

In scope:

- Add a public batch zero-copy fetch API on `KVNode`
- Reuse the existing `fetch_to()` / `fetch_to_impl()` data path
- Let the caller define the output layout inside a single `MemoryRegion`
- Return per-key success/failure information
- Validate overlapping/out-of-bounds region layout before issuing any fetches

Out of scope:

- New server protocol messages
- New wire protocol for fetch
- Automatic output layout planning
- Cross-key scheduling by owner
- Multi-owner concurrency control
- Subscription-integrated batch zero-copy helpers
- New metrics structs for batch fetch in Phase 1

## API

Add the following public types and method to `axon::kv`.

```cpp
struct FetchToItem {
    std::string key;
    size_t length = 0;
    size_t offset = 0;
};

struct FetchToManyResult {
    std::vector<std::string> completed;
    std::vector<std::string> failed;
    bool all_succeeded = false;
};

FetchToManyResult fetch_to_many(const std::vector<FetchToItem>& items,
                                const axon::MemoryRegion::Ptr& region);
```

## Semantics

### `FetchToItem`

Each item describes where a single key should be written inside the caller's
registered output region.

- `key`: key to fetch
- `length`: available output capacity for this key
- `offset`: byte offset into the provided `MemoryRegion`

Phase 1 requires the caller to provide a complete layout. AXON does not pack or
reorder items.

`length` is a caller-provided capacity bound, not the fetched object's true
size. It must be large enough for the actual value size of the key. If the
published value is larger than `length`, the per-key fetch fails in the normal
`fetch_to()` path.

### `fetch_to_many`

- The operation is synchronous from the caller's point of view.
- The method validates the whole item list before issuing any network fetches.
- Validation includes:
  - non-empty item list
  - non-empty keys
  - non-zero lengths
  - non-null region
  - `offset + length` inside the region bounds
  - no overlapping output ranges across items
- After validation, the implementation processes items one by one using the
  existing `fetch_to()` path.
- Each item either:
  - completes successfully and is appended to `completed`, or
  - fails and is appended to `failed`
- `all_succeeded` is true only if every item succeeds.

## Error model

This API is a batch helper, not an all-or-nothing transaction.

- Validation failures are immediate API-level failures and should throw or
  return the existing `Status` error through the same conventions used by other
  synchronous helper APIs.
- Per-key fetch failures do not abort the whole batch in Phase 1.
- If an item fails after earlier items succeeded, the earlier writes remain
  valid in the caller's region.

This gives the caller partial progress rather than discarding already-fetched
data.

## Layout rules

The output layout rules are strict:

- Each item writes to `[offset, offset + length)`
- The ranges must not overlap
- The ranges must fit fully inside `region->length()`

The first implementation should validate overlap by sorting the requested
intervals by offset and performing a linear scan over adjacent ranges.

If any rule is violated, the whole call fails before issuing any fetches.

This keeps the API deterministic and avoids partially-corrupted caller memory.

## Phase 1 execution strategy

Phase 1 intentionally stays simple:

1. validate all items
2. iterate items in caller-provided order
3. call `fetch_to(key, region, length, offset)` for each item
4. aggregate `completed` / `failed`
5. return the batch result

This means Phase 1 is a public batch zero-copy API, but not yet a fully
optimized multi-request scheduler.

## Relationship to existing APIs

- `fetch(key)`:
  - returns owning bytes
  - includes result-copy cost
- `fetch_to(key, region, length, offset)`:
  - single-key zero-copy fetch into caller memory
- `fetch_to_many(items, region)`:
  - multi-key zero-copy fetch into caller memory

`fetch_to_many()` is the natural batch companion to `fetch_to()`.

## Future evolution

Phase 1 is deliberately minimal. Later phases may add:

1. owner-aware grouping
2. concurrent outstanding fetches
3. subscription-driven zero-copy batch helpers
4. automatic layout planning helpers

Possible future APIs:

```cpp
BatchFetchToResult subscribe_and_fetch_to_once_many(...);
std::vector<FetchToPlanItem> plan_fetch_layout(...);
```

These are explicitly out of scope for this phase.

## Tests

Phase 1 should cover at least:

1. single-region multi-key happy path
2. one failed key with others succeeding
3. overlapping layout rejection
4. out-of-bounds layout rejection
5. duplicate keys writing to distinct offsets
6. empty item list rejection

## Notes

- This design does not change `fetch_to()` wire behavior.
- This design does not require server changes.
- This design is compatible with the current single-`data_addr` model.
