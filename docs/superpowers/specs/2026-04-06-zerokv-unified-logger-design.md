# ZeroKV Unified Logger Design

## Summary

Introduce a unified internal logger for the ZeroKV library. The logger is for
library-internal diagnostics only and does not change example or benchmark
stdout/stderr user-facing output.

This design standardizes:

- human-readable single-line log output
- environment-variable-driven log level control
- one shared output path for existing performance trace lines and future fault
  diagnosis logs

The initial scope covers:

- `zerokv::KV`
- `zerokv::core::*`
- `zerokv::transport::*`

Out of scope:

- changing example/benchmark output formatting
- adding metrics/export systems
- adding `Config`-level logging controls


## Goals

1. Provide one internal logging framework instead of ad hoc trace helpers.
2. Support log levels controlled by environment variable.
3. Keep output human-readable and easy to grep.
4. Allow existing performance tracing to become `trace`-level logs.
5. Create a clean place to add lower-level fault diagnosis logs in
   `core`/`transport`.


## Non-Goals

- No structured JSON logging in this phase.
- No Prometheus/OpenTelemetry integration.
- No changes to public library API.
- No changes to example CLI output or benchmark row formatting.


## Log Level Model

Environment variable:

- `ZEROKV_LOG_LEVEL`

Supported values:

- `error`
- `warn`
- `info`
- `debug`
- `trace`

Default:

- `error`

Semantics:

- `error`: operation failure, unrecoverable condition, or externally visible
  failure path
- `warn`: degraded behavior, fallback path, retry-heavy condition, best-effort
  cleanup failure
- `info`: important lifecycle transitions worth seeing during normal diagnosis
- `debug`: detailed control-flow state useful for debugging
- `trace`: high-frequency performance/stage logs, including current perf tracing


## Output Format

Output remains human-readable single-line text.

Format:

```text
[zerokv][<level>][<component>] <message>
```

Examples:

```text
[zerokv][error][transport.endpoint] ucp_get_nbx failed: status=kTransportError peer=10.0.0.2:5000
[zerokv][warn][core.kv_node] subscribe cleanup failed: key=foo code=kConnectionReset
[zerokv][trace][kv.sender] KV_SEND_SYNC_TOTAL key=foo bytes=1048576 total_us=17699 status=0
```

Notes:

- Existing trace payload text can remain mostly intact.
- The logger prepends level and component.
- Single-line output is serialized through one shared sink to avoid interleaved
  partial lines.


## Architecture

Add one internal logger facility under `src/internal/`.

Planned pieces:

- `src/internal/logging.h`
- `src/internal/logging.cpp`

Responsibilities:

- parse `ZEROKV_LOG_LEVEL` once
- expose level enum
- expose level-enabled check
- provide line emission with shared mutex
- provide lightweight helpers/macros for library-internal use

Suggested internal API:

```cpp
namespace zerokv::detail {

enum class LogLevel : uint8_t {
    kError,
    kWarn,
    kInfo,
    kDebug,
    kTrace,
};

LogLevel current_log_level();
bool log_enabled(LogLevel level);
void write_log_line(LogLevel level, std::string_view component, std::string_view message);

}  // namespace zerokv::detail
```

Minimal convenience macros/helpers are acceptable if they do not leak into the
public API.


## Migration Strategy

### Phase 1

Introduce the logger and route existing trace helpers through it:

- `trace_message_kv(...)`
- `trace_kv(...)`

Behavior:

- `ZEROKV_MESSAGE_KV_TRACE` and `ZEROKV_KV_TRACE` remain temporarily supported
  as compatibility gates during migration
- internally, when they emit, they do so via the shared logger at `trace` level

This avoids a flag day while giving one output path.

### Phase 2

Add true fault-diagnosis logs in lower layers:

- `transport::Endpoint`
- `transport::Worker`
- `core::KVNode`
- TCP control path helpers

Focus on:

- connection/open/close failures
- UCX request failures
- subscription/control RPC failures
- cleanup failures
- fallback path activation

### Phase 3

Retire old ad hoc trace env handling once the unified logger fully covers the
same use cases.


## Component Naming

Use stable component names for grepability:

- `kv.sender`
- `kv.receiver`
- `kv.cleanup`
- `core.kv_node`
- `core.kv_server`
- `core.tcp`
- `transport.endpoint`
- `transport.worker`
- `transport.memory`
- `transport.future`


## Initial Fault Logging Scope

After the logger exists, the first lower-layer logs to add should be:

1. `transport.endpoint`
- failed `ucp_tag_send_nbx`
- failed `ucp_tag_recv_nbx`
- failed `ucp_get_nbx`
- failed `ucp_put_nbx`
- endpoint close/reset conditions

2. `core.kv_node`
- subscribe/unsubscribe failures
- metadata RPC failures
- wait-any fallback activation when useful

3. `core.tcp`
- connect/send/read frame failures in control path

This design intentionally keeps the initial migration small and staged.


## Compatibility

No public API changes.

No changes to:

- `KV`
- `core::*`
- `transport::*`
- example CLI flags

Library users who do not set `ZEROKV_LOG_LEVEL` should see only `error` logs.


## Risks

1. Too much output at `trace`
- acceptable
- `trace` is opt-in

2. Mixing legacy trace env vars with `ZEROKV_LOG_LEVEL`
- acceptable during migration
- implementation should document precedence clearly

3. Logging overhead in hot paths
- keep string building behind level checks where necessary
- high-frequency logs remain opt-in


## Precedence Rules

During transition:

- `ZEROKV_LOG_LEVEL=trace` enables all trace output through the unified logger
- legacy trace env vars may continue to force emission for their current call
  sites even if `ZEROKV_LOG_LEVEL` is unset

Implementation should keep this simple:

- legacy perf trace sites keep working
- unified logger becomes the common sink


## Testing

1. Unit test log level parsing:
- unset defaults to `error`
- known values parse correctly
- unknown values fall back to `error`

2. Unit test level filtering:
- `warn` suppresses `info/debug/trace`
- `trace` enables everything

3. Focused library test:
- existing perf trace paths still emit when legacy env vars are set

4. Focused logger sink test:
- multiple threads writing through logger produce complete single lines


## Rollout

Recommended implementation order:

1. Add internal logger facility
2. Route existing trace helpers through it
3. Add minimal tests
4. Add first lower-layer fault logs in `transport.endpoint` and `core.kv_node`
5. Document env var behavior
