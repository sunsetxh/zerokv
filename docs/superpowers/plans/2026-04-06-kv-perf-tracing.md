# KV Performance Tracing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Standardize existing `KV` and `KVNode` trace output into a consistent phase-oriented performance trace so benchmark results can be correlated with internal sender, cleanup, and receiver phases without one-off instrumentation.

**Architecture:** Keep `ZEROKV_MESSAGE_KV_TRACE` and `ZEROKV_KV_TRACE` as the only toggles. Normalize hot-path trace names and fields in `src/kv.cpp` and `src/core/node.cpp`, and verify the resulting trace lines line up with `message_kv_demo` benchmark output. No new metrics objects, no API changes.

**Tech Stack:** C++20, existing `trace_message_kv()` / `trace_kv()` helpers, `message_kv_demo`, VM1/VM2 TCP/RDMA validation.

---

## File Map

- Modify: `src/kv.cpp`
  - normalize sender / async cleanup / recv batch trace lines
- Modify: `src/core/node.cpp`
  - normalize wait-any / wait-key trace lines
- Optional docs after validation: `docs/reports/current-zerokv-implementation.md`

## Task 1: Normalize sender and cleanup trace names in `src/kv.cpp`

**Files:**
- Modify: `src/kv.cpp`

- [ ] **Step 1: Enumerate current hot-path trace lines**

Audit current `trace_message_kv(...)` calls in `src/kv.cpp` and identify the
hot-path lines that must be renamed or normalized. At minimum cover:

- sender sync total
- sender async enqueue
- sender async batch cleanup
- lazy ack unsubscribe
- recv batch start/fetch/ack/complete

- [ ] **Step 2: Rename sender trace families**

Standardize sender-side event names, for example:

- `KV_SEND_ASYNC_ENQUEUE`
- `KV_SEND_ASYNC_COMPLETE`
- `KV_SEND_SYNC_TOTAL`
- `KV_SEND_REGION_SYNC_TOTAL`

Ensure fields use consistent names such as:

- `key`
- `bytes`
- `total_us`
- `status`

- [ ] **Step 3: Rename cleanup trace families**

Standardize async cleanup lines, for example:

- `KV_ASYNC_CLEANUP_DRAIN`
- `KV_ASYNC_CLEANUP_WAIT_ANY`
- `KV_ASYNC_CLEANUP_READY`
- `KV_ASYNC_CLEANUP_UNPUBLISH`
- `KV_ASYNC_CLEANUP_COMPLETE`
- `KV_ASYNC_CLEANUP_DEFER_ACK_UNSUBSCRIBE`

Where useful, add:

- `active`
- `queued`
- `source=event|fallback`
- `cleanup_us`

- [ ] **Step 4: Normalize recv batch trace families**

Standardize `recv_batch()` traces, for example:

- `KV_RECV_BATCH_BEGIN`
- `KV_RECV_BATCH_WAIT_ANY`
- `KV_RECV_BATCH_FETCH_BEGIN`
- `KV_RECV_BATCH_FETCH_DONE`
- `KV_RECV_BATCH_ACK_BEGIN`
- `KV_RECV_BATCH_ACK_DONE`
- `KV_RECV_BATCH_COMPLETE`

- [ ] **Step 5: Keep scope narrow**

Do not redesign code structure just to emit traces. This task is trace naming
and field normalization only.

- [ ] **Step 6: Commit**

```bash
git add src/kv.cpp
git commit -m "Normalize KV sender and recv performance traces"
```

## Task 2: Normalize `KVNode` wait traces in `src/core/node.cpp`

**Files:**
- Modify: `src/core/node.cpp`

- [ ] **Step 1: Rename wait-any traces**

Standardize:

- `KV_WAIT_ANY_BEGIN`
- `KV_WAIT_ANY_MATCH`
- `KV_WAIT_ANY_TIMEOUT`

Reuse fields like:

- `key`
- `keys`
- `timeout_ms`
- `source`

- [ ] **Step 2: Normalize wait-key traces**

Standardize `wait_for_key`/`wait_for_keys` traces so they complement, rather
than duplicate, higher-level `KV` traces. Suggested names:

- `KV_WAIT_KEY_BEGIN`
- `KV_WAIT_KEY_DONE`

- [ ] **Step 3: Preserve lower-level role**

These traces should explain why a higher-level phase took time, not duplicate
all `KV` lifecycle events.

- [ ] **Step 4: Commit**

```bash
git add src/core/node.cpp
git commit -m "Normalize KVNode wait performance traces"
```

## Task 3: Validate trace usefulness on VM1/VM2

**Files:**
- No required code changes
- Optional docs update after validation

- [ ] **Step 1: VM1 build smoke**

Run:

```bash
cmake --build build -j4 --target test_message_kv message_kv_demo
```

- [ ] **Step 2: Manual trace validation on VM1/VM2**

Run `message_kv_demo` with:

- `ZEROKV_MESSAGE_KV_TRACE=1`
- `ZEROKV_KV_TRACE=1`
- TCP and RDMA
- `--sizes 64K,1M`
- `--send-mode sync` and `--send-mode async`

Confirm:

- sender lines appear in intuitive order
- cleanup lines explain async completion behavior
- receiver lines explain `RECV_ROUND` timing
- lower-level wait traces explain event/fallback decisions

- [ ] **Step 3: Check benchmark interpretability**

Ensure the following benchmark questions can now be answered from the trace
without ad-hoc temporary instrumentation:

- why was sender async `send_us` low but `SEND_ROUND total_us` still high?
- did cleanup progress come from event or fallback?
- did receiver spend time waiting, fetching, or acking?

- [ ] **Step 4: Commit validation-only notes if docs are updated**

If you update the report, use a separate commit:

```bash
git add docs/reports/current-zerokv-implementation.md
git commit -m "Document KV performance tracing conventions"
```
