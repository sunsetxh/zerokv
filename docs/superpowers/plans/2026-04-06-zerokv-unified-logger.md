# ZeroKV Unified Logger Implementation Plan

## Goal

Introduce a unified internal logger for library-internal diagnostics with:

- human-readable single-line output
- environment-variable-driven log levels via `ZEROKV_LOG_LEVEL`
- one shared sink for current perf traces and future fault logs

This plan only covers library-internal logging. Example and benchmark stdout/stderr
output remains unchanged.


## Task 1: Add internal logger facility

Scope:

- add `src/internal/logging.h`
- add `src/internal/logging.cpp`
- extend current shared sink behavior from `trace_log.h` into a real logger

Requirements:

- define `LogLevel` enum:
  - `error`, `warn`, `info`, `debug`, `trace`
- parse `ZEROKV_LOG_LEVEL` once
- default to `error`
- unknown values fall back to `error`
- expose:
  - `current_log_level()`
  - `log_enabled(LogLevel)`
  - `write_log_line(LogLevel, component, message)`
- preserve one shared mutex-protected output path
- output format:
  - `[zerokv][<level>][<component>] <message>`

Testing:

- add focused unit tests for log-level parsing/filtering
- verify fallback-to-error behavior for unknown values


## Task 2: Route existing perf trace helpers through unified logger

Scope:

- update `src/kv.cpp`
- update `src/core/node.cpp`
- keep current trace semantics and env vars working

Requirements:

- `trace_message_kv(...)` emits through unified logger at:
  - level `trace`
  - component names such as `kv.sender`, `kv.receiver`, `kv.cleanup`
- `trace_kv(...)` emits through unified logger at:
  - level `trace`
  - component names such as `core.kv_node`
- keep legacy trace env vars temporarily supported:
  - `ZEROKV_MESSAGE_KV_TRACE`
  - `ZEROKV_KV_TRACE`
- do not change trace payload meaning

Performance requirement:

- avoid unnecessary string construction when disabled
- use explicit `if (log_enabled(...))` guards or equivalent helper wrappers at
  hot call sites before building trace strings

Testing:

- rebuild focused targets
- verify existing perf trace tests still pass


## Task 3: Add first lower-layer fault diagnosis logs

Scope:

- `src/transport/endpoint.cpp`
- `src/core/node.cpp`
- only high-signal fault and fallback paths

Requirements:

- add `error` logs for:
  - UCX send/recv/get/put submission failures
  - endpoint connection/close/reset failures where useful
  - metadata/control RPC failures
- add `warn` logs for:
  - fallback path activation
  - cleanup retry/failure paths
  - subscribe/unsubscribe cleanup failures
- keep logs concise and single-line
- use stable component names:
  - `transport.endpoint`
  - `core.kv_node`
  - `core.tcp` if needed

Testing:

- focused unit/integration rebuild
- smoke-run representative tests that exercise existing error/fallback paths


## Task 4: Validation

Validation checklist:

- VM1 build:
  - `test_message_kv`
  - `test_kv_node`
  - `test_future`
- focused tests pass
- manual smoke check with env vars:
  - `ZEROKV_LOG_LEVEL=trace`
  - confirm unified prefix format
- manual smoke check:
  - `ZEROKV_LOG_LEVEL=warn`
  - confirm trace lines are suppressed


## Review Gates

1. Task 1 review
- logger facility only

2. Task 2 review
- trace migration only

3. Task 3 review
- lower-layer fault logs only

4. Task 4 validation review
- no code changes


## Risks

1. Hot-path overhead
- mitigated by guarding string construction behind level-enabled checks

2. Log spam at `trace`
- acceptable because `trace` is opt-in

3. Mixed old/new trace semantics during migration
- acceptable in this phase
- old env vars remain compatibility switches


## Completion Criteria

- unified internal logger exists and is used as the common sink
- `ZEROKV_LOG_LEVEL` works with the five defined levels
- current perf trace helpers emit through the unified logger
- first batch of lower-layer fault logs exists in `core/transport`
- focused validation passes
