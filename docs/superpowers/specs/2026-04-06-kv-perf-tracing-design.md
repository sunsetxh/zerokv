# KV Performance Tracing Design

## Overview

The current `ZEROKV_MESSAGE_KV_TRACE` and `ZEROKV_KV_TRACE` output is useful for
ad-hoc debugging, but it is not yet consistent enough to serve as a reliable
performance analysis surface.

Recent sender optimization work exposed this gap:

- some stages log total latency only
- some log partial phase latency
- naming is inconsistent across sync sender, async sender, cleanup, and recv
- correlating demo benchmark output with internal phases still requires manual
  reconstruction

This design keeps the existing environment-variable-driven tracing model, but
upgrades the output into a more structured phase-oriented performance trace.

## Goals

- Keep `ZEROKV_MESSAGE_KV_TRACE` and `ZEROKV_KV_TRACE` as the trace toggles
- Standardize trace naming and field layout across sender, cleanup, and receiver
- Make sender benchmark interpretation easier by exposing:
  - enqueue latency
  - ack wait latency
  - cleanup latency
  - round barrier behavior
- Make receiver benchmark interpretation easier by exposing:
  - subscription hit path
  - fetch start / fetch done
  - ack publish latency
- Preserve low implementation risk by staying in existing trace infrastructure

## Non-Goals

- No Prometheus/OpenTelemetry integration
- No new metrics service or background aggregator
- No public API changes
- No persistence or binary trace format
- No attempt to make traces stable ABI; they remain diagnostic output

## Current Problems

The current traces have four main issues:

1. inconsistent event naming
2. inconsistent field naming
3. mixed scope (some lines are phase-level, others are total-level)
4. incomplete phase coverage for benchmark interpretation

For example:

- sender sync path logs total send latency
- async path logs enqueue and completion separately
- cleanup path logs some wait details but not always in the same schema
- batch recv logs useful events but not with one obvious per-key phase model

## Proposed Model

Adopt a phase-oriented trace convention:

- one event name per lifecycle phase
- common field order when practical:
  - `key=...`
  - `bytes=...`
  - `phase=...` or phase-specific event name
  - `*_us=...`
  - `status=...`

The trace remains line-oriented text, but each line should be parse-friendly.

## Trace Families

### 1. Sender enqueue / completion

Use `ZEROKV_MESSAGE_KV_TRACE` for `KV` sender lifecycle:

- `KV_SEND_ASYNC_ENQUEUE`
- `KV_SEND_ASYNC_COMPLETE`
- `KV_SEND_SYNC_TOTAL`
- `KV_SEND_REGION_SYNC_TOTAL`

These should make the difference between:

- async thread return latency
- async final completion latency
- sync end-to-end blocking latency

explicit.

### 2. Async cleanup

For sender cleanup thread:

- `KV_ASYNC_CLEANUP_DRAIN`
- `KV_ASYNC_CLEANUP_WAIT_ANY`
- `KV_ASYNC_CLEANUP_READY`
- `KV_ASYNC_CLEANUP_UNPUBLISH`
- `KV_ASYNC_CLEANUP_COMPLETE`
- `KV_ASYNC_CLEANUP_DEFER_ACK_UNSUBSCRIBE`

These should let us answer:

- how many sends were active
- whether completion came from event or fallback
- how expensive message-key deletion is

### 3. Receiver batch phases

For `recv_batch()`:

- `KV_RECV_BATCH_BEGIN`
- `KV_RECV_BATCH_WAIT_ANY`
- `KV_RECV_BATCH_FETCH_BEGIN`
- `KV_RECV_BATCH_FETCH_DONE`
- `KV_RECV_BATCH_ACK_BEGIN`
- `KV_RECV_BATCH_ACK_DONE`
- `KV_RECV_BATCH_COMPLETE`

These should make the per-key receive lifecycle visible without having to infer
it from mixed logs.

### 4. Core wait-any / fallback path

Keep `ZEROKV_KV_TRACE` for lower-level `KVNode` wait behavior:

- `KV_WAIT_ANY_BEGIN`
- `KV_WAIT_ANY_MATCH`
- `KV_WAIT_ANY_TIMEOUT`
- `KV_WAIT_KEY_BEGIN`
- `KV_WAIT_KEY_DONE`

The purpose here is not to duplicate higher-level traces, but to explain why a
sender or receiver phase took the time it did.

## Field Conventions

Preferred fields:

- `key=<message key or ack key>`
- `ack_key=<...>` when relevant
- `bytes=<payload bytes>`
- `active=<count>`
- `queued=<count>`
- `wait_us=<...>`
- `fetch_us=<...>`
- `ack_us=<...>`
- `cleanup_us=<...>`
- `total_us=<...>`
- `status=<error code int or 0>`
- `source=event|fallback` when a ready condition can come from either path

Not every line needs every field, but field names should be reused consistently.

## Demo Relationship

`message_kv_demo` should continue to print its existing benchmark summaries, but
the internal trace should now line up with those summaries better.

Specifically:

- `SEND_OK ... send_us=...`
  should map cleanly to sender enqueue or sync total behavior
- `SEND_ROUND total_us=...`
  should be explainable from cleanup and barrier traces
- `RECV_ROUND recv_total_us=...`
  should be explainable from batch wait/fetch/ack traces

No CLI change is required in this phase.

## Implementation Scope

This phase should stay narrow:

- rename / normalize existing trace lines
- add missing phase lines where benchmark interpretation currently has blind
  spots
- do not redesign code structure just to emit traces

It is acceptable to leave some legacy trace line names in place temporarily if
they are not on hot paths, but the sender async path, sender sync path, and
batch recv path should be coherent by the end of this work.

## Testing

1. Focused trace formatting tests where practical
- helper-level tests for demo summary strings remain unchanged
- no need for golden full-trace snapshots if they would be brittle

2. Manual trace validation on VM1/VM2
- run TCP and RDMA `message_kv_demo`
- confirm key sender and receiver phases appear in the expected order

3. Benchmark interpretation check
- confirm that recent sender optimization scenarios can be explained with the
  new trace set without adding one-off instrumentation

## Expected Outcome

After this work:

- sender and receiver phase timing should be easier to compare across commits
- async sender improvements should be easier to attribute
- future performance work should need fewer one-off trace patches
