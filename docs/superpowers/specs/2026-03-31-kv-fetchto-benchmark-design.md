# KV FetchTo and Benchmark Metrics Design

**Date:** 2026-03-31

## Goal

Clarify `fetch_to()` as the primary zero-copy public fetch API, fix fetch metric
accounting gaps, add a data-plane-oriented `bench-fetch-to` benchmark mode, and
record the current multi-NIC research as a follow-up TODO.

## Scope

In scope:

- document `fetch_to()` as the primary zero-copy fetch API
- keep `fetch()` as the convenience API that copies into a returned
  `FetchResult`
- add `FetchMetrics::result_copy_us`
- keep `FetchMetrics::total_us` as end-to-end wall-clock time
- rename benchmark throughput columns from `throughput_MBps` to
  `throughput_MiBps`
- add `bench-fetch-to` to `kv_bench`
- add documentation/TODO notes for UCX multi-rail vs. current ZeroKV single
  `data_addr` model

Out of scope:

- changing `fetch_to()` wire protocol
- changing `fetch()` return type
- changing server metadata schema for multi-NIC support
- implementing multi-NIC striping or multi-address registration
- push benchmark changes

## Current Problem

The current `fetch()` benchmark numbers are hard to interpret for real RDMA
performance work because:

- `FetchMetrics` does not include the final copy from the temporary RDMA buffer
  into `FetchResult::data`
- `bench-fetch` therefore reports a `total_us` that is larger than the visible
  stage sums
- the benchmark table label `throughput_MBps` is misleading because the code
  divides by `1024 * 1024`, so the actual unit is `MiB/s`
- `bench-fetch` measures application-level end-to-end fetch cost, not a
  zero-copy data-plane-only path

This caused confusion during real NIC validation and made UCX multi-rail
experiments difficult to interpret.

## User-Facing Design

### Public API Positioning

`KVNode` will continue to expose both:

- `fetch(key)` — convenience API
- `fetch_to(key, region, length, offset)` — zero-copy API

But the documentation will explicitly position them differently:

- `fetch()` is the convenience API for callers that want owned bytes returned
  as a `FetchResult`
- `fetch_to()` is the primary zero-copy public API for performance-sensitive
  paths and benchmark/production code that already manages destination memory

No signature changes are required in this phase.

### Fetch Metrics

Extend `FetchMetrics` with:

```cpp
uint64_t result_copy_us = 0;
```

Field semantics:

- `total_us`
  - full `fetch()` or `fetch_to()` wall-clock time
- `local_buffer_prepare_us`
  - local destination region allocation or validation
- `get_meta_rpc_us`
  - metadata lookup RPC
- `peer_connect_us`
  - peer endpoint lookup/connect path
- `rdma_prepare_us`
  - rkey construction and `get()` setup
- `rdma_get_us`
  - blocking RDMA transfer completion wait
- `result_copy_us`
  - copy from temporary local region into `FetchResult::data`
  - `0` for `fetch_to()`

This keeps `total_us` as the operator-facing end-to-end metric while making the
missing time visible and attributable.

### Benchmark Modes

Keep existing modes:

- `server`
- `hold-owner`
- `bench-publish`
- `bench-fetch`

Add:

- `bench-fetch-to`

Semantics:

- `bench-fetch`
  - application-level end-to-end benchmark
  - uses `fetch()`
  - includes final result copy in `total_us`
- `bench-fetch-to`
  - zero-copy benchmark
  - pre-allocates one local `MemoryRegion`
  - repeatedly calls `fetch_to()`
  - reports a cleaner data-plane-oriented cost profile

The same size sweep and `--iters` / `--total-bytes` behavior apply to
`bench-fetch-to`.

### Throughput Units

Benchmark output headers will be renamed from:

- `throughput_MBps`

to:

- `throughput_MiBps`

This matches the actual implementation, which divides by `1024 * 1024`.

## Multi-NIC Research Note

The current findings should be preserved as an explicit TODO:

- UCX itself supports multi-rail / multi-NIC usage
- current ZeroKV KV metadata registers only one `data_addr` per node
- therefore ZeroKV currently benefits only from whatever UCX can do behind a
  single registered endpoint
- ZeroKV does not yet model:
  - multiple `data_addr` entries per node
  - per-key NIC selection
  - per-object striping across multiple NICs

The follow-up TODO should recommend this evolution order:

1. multiple `data_addr` registration
2. per-key single-NIC selection
3. optional per-object multi-segment striping

## Implementation Notes

### `fetch()`

`fetch()` should keep its current behavior:

1. metadata lookup
2. allocate temporary local region
3. `fetch_to_impl(...)`
4. allocate/resize `FetchResult::data`
5. `memcpy` from local region into result vector

The new `result_copy_us` must measure steps 4-5.

### `fetch_to()`

`fetch_to()` should keep `result_copy_us = 0` and remain the better benchmark
surface for pure RDMA fetch analysis.

### `bench-fetch-to`

For each size:

1. derive iteration count
2. allocate one reusable `MemoryRegion` large enough for the size
3. for each iteration:
   - call `fetch_to(key, region, size, 0)`
   - validate success
   - read `last_fetch_metrics()`
4. report averages using the same table shape as `bench-fetch`

The benchmark does not need to validate payload contents in this phase; size
and successful completion are sufficient.

## Testing

Required coverage:

1. `fetch()` records non-zero `result_copy_us`
2. `fetch_to()` leaves `result_copy_us == 0`
3. benchmark output uses `throughput_MiBps`
4. `bench-fetch-to` smoke test runs for a small local size
5. documentation/TODO updated to mention:
   - `fetch_to()` as zero-copy public API
   - multi-NIC follow-up direction

## Risks

- `bench-fetch-to` is still not a pure wire-time benchmark because it includes
  metadata lookup and endpoint reuse/setup costs
- UCX multi-rail may still show little benefit under the current single-endpoint
  application design
- large real-NIC fetch measurements may still require a later “data-only”
  benchmark mode if operators want to exclude lookup entirely
