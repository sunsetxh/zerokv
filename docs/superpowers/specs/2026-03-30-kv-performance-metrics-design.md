# KV Performance Metrics Design

**Date:** 2026-03-30

## Goal

Add first-pass end-to-end performance metrics for the KV MVP so callers can
inspect the latest `publish` and `fetch` timing breakdowns without changing the
existing async result types.

## Scope

In scope:

- metrics for `KVNode::publish()`
- metrics for `KVNode::publish_region()`
- metrics for `KVNode::fetch()`
- metrics for `KVNode::fetch_to()`
- public read-only accessors on `KVNode`
- lightweight metric printing in `kv_demo`
- unit/integration coverage for successful and failed metric recording

Out of scope:

- aggregated histograms or percentiles
- persistent metrics history
- global metrics registry
- tracing infrastructure shared by the whole library
- benchmark mode or structured export formats
- direct-put and subscription work

## User-Facing Design

The API stays result-oriented: `publish()` and `fetch()` keep their current
return types. Metrics are exposed through separate `KVNode` query methods that
return the most recent publish/fetch observation.

New public types:

```cpp
struct PublishMetrics {
    uint64_t total_us = 0;
    uint64_t prepare_region_us = 0;
    uint64_t pack_rkey_us = 0;
    uint64_t put_meta_rpc_us = 0;
    bool ok = false;
};

struct FetchMetrics {
    uint64_t total_us = 0;
    uint64_t local_buffer_prepare_us = 0;
    uint64_t get_meta_rpc_us = 0;
    uint64_t peer_connect_us = 0;
    uint64_t rdma_prepare_us = 0;
    uint64_t rdma_get_us = 0;
    bool ok = false;
};
```

New `KVNode` methods:

```cpp
[[nodiscard]] std::optional<PublishMetrics> last_publish_metrics() const;
[[nodiscard]] std::optional<FetchMetrics> last_fetch_metrics() const;
```

These accessors return the last completed operation of that category. The value
is overwritten by the next publish or fetch attempt. Failed operations still
record completed stages and set `ok = false`.

## Metrics Semantics

### Publish path

- `prepare_region_us`
  - time spent allocating/copying or validating the local `MemoryRegion`
- `pack_rkey_us`
  - time spent obtaining the packed `RemoteKey`
- `put_meta_rpc_us`
  - full request/response time for the server metadata registration RPC
- `total_us`
  - full publish wall-clock time

### Fetch path

- `local_buffer_prepare_us`
  - time spent allocating or validating the destination `MemoryRegion`
- `get_meta_rpc_us`
  - full request/response time for metadata lookup
- `peer_connect_us`
  - time spent retrieving or establishing the peer endpoint
- `rdma_prepare_us`
  - time spent preparing the RDMA get request after metadata lookup, including
    building the `RemoteKey` object and any immediate pre-transfer setup
- `rdma_get_us`
  - time spent waiting for the RDMA get to complete
- `total_us`
  - full fetch wall-clock time

If a stage does not run because a request fails earlier, its field remains `0`.

## Internal Design

The implementation remains intentionally simple:

- store `last_publish_metrics_` and `last_fetch_metrics_` in `KVNode::Impl`
- guard them with a small mutex dedicated to metrics reads/writes
- measure stages with `std::chrono::steady_clock`
- write helpers that convert elapsed durations into microseconds

No ring buffer or multi-sample history is added in this phase. That keeps the
new state isolated and avoids turning the MVP transport path into a metrics
framework.

## Error Handling

Metrics are best-effort and must never change the success/failure behavior of
the operation itself.

Rules:

- metric recording must not throw
- API callers always see the original publish/fetch status
- failure paths still update the last metrics snapshot when timing data exists
- `last_*_metrics()` returns `std::nullopt` until the first operation of that
  category completes or fails

## Threading Model

The current KV MVP mostly uses synchronous operations guarded by existing mutexes.
Metrics add one more low-contention mutex for snapshot state:

- publish/fetch code records metrics after each operation attempt
- readers calling `last_publish_metrics()` or `last_fetch_metrics()` take the
  same mutex and read a copy

Lock ordering is explicit:

- `control_mu_ -> published_mu_ -> metrics_mu_`
- metric snapshots must be written only after releasing any other operation lock

This is enough for the current single-node observation use case and does not
change endpoint or control-plane synchronization.

## Demo Behavior

`examples/kv_demo.cpp` should print a concise summary after publish/fetch:

- publish: total, region prepare, rkey pack, metadata RPC
- fetch: total, local buffer prepare, metadata RPC, peer connect, rkey unpack,
  RDMA prepare, RDMA get

The demo remains human-readable; no new CLI switches are required.

## Testing Strategy

Add tests for:

1. publish metrics recorded on successful copy-publish
2. publish metrics overwritten by the next publish call
3. fetch metrics recorded across two nodes with non-zero RPC and RDMA stages
4. failed fetch records failure status and preserves lookup timing

Verification commands should continue to use the existing KV integration test
targets plus targeted test filtering for new cases.

## Risks and Non-Goals

Known trade-offs in this phase:

- metrics are last-sample only
- timings include library overhead and are not wire-time only
- peer connect timing includes endpoint cache miss cost, which is intentional
- later benchmark/export work may require a separate metrics history layer

This phase is explicitly for observability of the current MVP, not for building
a generic telemetry subsystem.
