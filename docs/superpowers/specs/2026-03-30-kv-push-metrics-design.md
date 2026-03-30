# KV Push Metrics Design

**Date:** 2026-03-30

## Goal

Add first-pass performance metrics for the KV `push()` path so callers can
inspect the latest push timing breakdown without changing the existing async
result type.

## Scope

In scope:

- metrics for `KVNode::push()`
- public read-only accessor on `KVNode`
- lightweight metric printing in `kv_demo`
- integration coverage for successful and failed push metric recording

Out of scope:

- persistent history or ring buffers
- aggregated statistics or percentiles
- global metrics registry
- benchmark mode or structured metrics export
- subscription metrics
- control-plane redesign for asynchronous push RPCs

## User-Facing Design

The API stays result-oriented: `push()` keeps returning `Future<void>`. Metrics
are exposed through a separate query method that returns the most recent push
observation.

New public type:

```cpp
struct PushMetrics {
    uint64_t total_us = 0;
    uint64_t get_target_rpc_us = 0;
    uint64_t prepare_frame_us = 0;
    uint64_t rdma_put_flush_us = 0;
    uint64_t commit_rpc_us = 0;
    bool ok = false;
};
```

New `KVNode` method:

```cpp
[[nodiscard]] std::optional<PushMetrics> last_push_metrics() const;
```

Semantics:

- returns the most recent push attempt
- the snapshot is overwritten by the next push attempt
- failed pushes still record completed stages and set `ok = false`
- `std::nullopt` is returned until the first push attempt completes or fails

## Metrics Semantics

- `get_target_rpc_us`
  - full request/response time for `GET_PUSH_TARGET`
- `prepare_frame_us`
  - time spent allocating the local frame region and writing the
    `[PushInboxHeader][key][payload]` frame
- `rdma_put_flush_us`
  - time spent waiting for RDMA `put` completion and the subsequent `flush`
    that makes the inbox visible before commit
- `commit_rpc_us`
  - time spent connecting to the target push-control listener, sending
    `PUSH_COMMIT`, and reading the response
- `total_us`
  - full wall-clock time for the push operation

If a stage does not run because a push fails earlier, its field remains `0`.

## Internal Design

The implementation mirrors the existing publish/fetch metrics approach:

- store `last_push_metrics_` in `KVNode::Impl`
- guard it with the existing dedicated metrics mutex
- measure stages with `std::chrono::steady_clock`
- update the snapshot once per push attempt

No history buffer is added in this phase. The goal is local observability, not
to build a telemetry subsystem.

## Error Handling

Metrics are best-effort and must never change push success/failure behavior.

Rules:

- metric recording must not throw
- callers always see the original push status
- failure paths still update the last metrics snapshot when timing data exists
- early validation failures record `total_us` and `ok = false`

## Threading Model

Push metrics reuse the existing `metrics_mu_`.

Lock ordering remains:

- `control_mu_ -> published_mu_ -> metrics_mu_`

Push metric snapshots must be written only after releasing any other operation
lock. As with publish/fetch metrics, callers read a copy under `metrics_mu_`.

## Demo Behavior

`examples/kv_demo.cpp` should print a concise push summary after success:

- total
- target lookup RPC
- local frame preparation
- RDMA put + flush
- commit RPC

The demo remains human-readable and does not add new CLI switches.

## Testing Strategy

Add tests for:

1. push metrics recorded on a successful sender -> target push
2. push metrics recorded on target lookup failure
3. push metrics overwritten by the next push attempt

Verification should reuse the existing KV integration test targets on VM1.

## Risks and Non-Goals

Known trade-offs in this phase:

- metrics are last-sample only
- `rdma_put_flush_us` intentionally combines the data write and visibility
  flush, because separating them adds little diagnostic value in this phase
- commit timing includes TCP connect cost for the short-lived push-control
  connection

This phase is for observing the current push path, not for building a generic
metrics framework.
