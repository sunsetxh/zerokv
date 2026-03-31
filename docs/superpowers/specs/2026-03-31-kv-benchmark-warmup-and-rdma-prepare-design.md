# KV Benchmark Warmup and RDMA Prepare Metrics Design

**Date:** 2026-03-31

## Goal

Improve fetch benchmark interpretability by:

- adding an optional warmup phase to benchmark modes
- splitting `rdma_prepare_us` into more meaningful sub-stages

This is intended to make small-message latency and data-plane preparation costs
easier to reason about during real RDMA validation.

## Scope

In scope:

- add `--warmup N` to `kv_bench`
- support warmup in:
  - `bench-publish`
  - `bench-fetch`
  - `bench-fetch-to`
- extend `FetchMetrics` with:
  - `rkey_prepare_us`
  - `get_submit_us`
- keep `rdma_prepare_us` for compatibility as:
  - `rkey_prepare_us + get_submit_us`
- update output tables, bindings, stubs, and docs

Out of scope:

- push benchmark warmup
- changing fetch semantics
- changing transport behavior
- adding percentile/histogram stats

## Current Problem

Recent real-NIC `bench-fetch-to` runs showed:

- very high `rdma_get_us` for `4KiB`
- non-trivial `rdma_prepare_us` for larger payloads

Two issues make the current output hard to interpret:

1. the benchmark has no explicit warmup control
2. `rdma_prepare_us` currently merges:
   - `RemoteKey` setup
   - `peer->get(...)` submit path

This prevents clear attribution of whether the cost comes from:

- one-time setup effects
- repeated request submission overhead
- rkey handling

## User-Facing Design

### Benchmark CLI

Add an optional flag:

```bash
--warmup N
```

Rules:

- default is `0`
- warmup applies per size
- warmup iterations run before measured iterations
- warmup results do not contribute to:
  - averaged metrics
  - throughput
  - printed iteration count

Example:

```bash
./kv_bench --mode bench-fetch-to \
  --server-addr 10.0.0.1:15000 \
  --data-addr 10.0.0.2:0 \
  --node-id bench-fetch-to \
  --sizes 4K,64K,1M \
  --warmup 2 \
  --iters 8 \
  --transport rdma
```

This means:

- each size runs 2 unmeasured warmup fetches
- then 8 measured fetches

### Fetch Metrics

Extend `FetchMetrics` with:

```cpp
uint64_t rkey_prepare_us = 0;
uint64_t get_submit_us = 0;
```

Semantics:

- `rkey_prepare_us`
  - time spent constructing the `RemoteKey` object from metadata
- `get_submit_us`
  - time spent in `peer->get(...)` before blocking on completion
- `rdma_prepare_us`
  - compatibility aggregate:
  - `rkey_prepare_us + get_submit_us`

This preserves existing consumers while exposing the more useful breakdown.

### Benchmark Output

Fetch-oriented tables (`bench-fetch` and `bench-fetch-to`) should print:

- `avg_rkey_prepare_us`
- `avg_get_submit_us`
- `avg_rdma_prepare_us`
- `avg_rdma_get_us`

`avg_rdma_prepare_us` remains in the table so old interpretation still works,
but the two new sub-columns explain where that time goes.

Publish table remains unchanged except for support for warmup.

## Implementation Notes

### Warmup Semantics

For each benchmark size:

1. run `warmup` iterations
2. discard their metrics
3. run measured iterations
4. average only measured iterations

If `--iters` is `0` or omitted:

- existing iteration derivation rules still apply
- warmup does not affect derived measured iteration count

Warmup should use the exact same code path as measured iterations to preserve
behavioral realism.

### `fetch_to_impl()`

The current flow is:

1. construct `RemoteKey`
2. call `peer->get(...)`
3. `get.get()`

The new timing split should be:

1. `rkey_prepare_us`
   - around `RemoteKey rkey; rkey.data = meta.rkey;`
2. `get_submit_us`
   - around `peer->get(...)`
3. `rdma_prepare_us`
   - sum of the two above
4. `rdma_get_us`
   - blocking completion wait

This keeps the instrumentation simple and directly aligned with the code.

## Testing

Required coverage:

1. `FetchMetrics` records:
   - non-zero `rkey_prepare_us`
   - non-zero `get_submit_us`
   - `rdma_prepare_us >= rkey_prepare_us + get_submit_us`
2. `bench-fetch-to` still works with `--warmup`
3. benchmark output renders the new columns
4. warmup iterations do not change reported `iters`

## Risks

- small measured iteration counts combined with warmup can still produce noisy
  small-message latency data
- `rkey_prepare_us` may stay very small compared to `get_submit_us`; that is
  still useful and should not be optimized away from the output
- keeping both split columns and aggregate columns slightly widens the table,
  but the added observability is worth it
