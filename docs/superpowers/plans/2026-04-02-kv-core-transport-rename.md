# KV / Core / Transport Rename Implementation Plan

## Scope

This plan executes the staged migration defined in:

- `docs/superpowers/specs/2026-04-02-kv-core-transport-rename-design.md`

Goals:

- promote `zerokv::KV` to the top-level user-facing API
- move current `zerokv::kv::*` infrastructure under `zerokv::core::*`
- move transport primitives under `zerokv::transport::*`
- reshape headers and source directories to match the new layering
- preserve compatibility temporarily with deprecated aliases and forwarding headers

Explicitly out of scope for this rename:

- `zerokv::Cluster` remains top-level
- `zerokv::plugin` remains top-level
- protocol semantics and runtime behavior changes unrelated to rename/layering

## Staging Principles

1. Do not do a flag-day rename in a single commit.
2. Introduce new locations before deleting old ones.
3. Preserve buildability after each task.
4. Keep compatibility aliases during phase 1.
5. Batch tests/examples separately from core file moves when practical.

## Task 1: Introduce top-level `KV` compatibility surface

Files:

- `include/zerokv/kv.h`
- `include/zerokv/message_kv.h`
- `src/message_kv.cpp`
- `include/zerokv/zerokv.h`
- `tests/integration/test_message_kv*.cpp`

Work:

- introduce `zerokv::KV` as the primary class name
- preserve `zerokv::MessageKV` as a deprecated alias in phase 1
- keep current implementation file in place for now; do not combine rename and file move yet
- update umbrella include to present `KV` as the intended top-level surface
- add or update focused tests proving both `KV` and compatibility alias compile

Acceptance:

- user code can instantiate `zerokv::KV`
- existing `MessageKV` tests still build
- compatibility alias is explicitly marked deprecated

## Task 2: Split current `include/zerokv/kv.h`

This is the highest-risk file move and must happen before namespace migration.

Files:

- `include/zerokv/kv.h`
- new `include/zerokv/core/kv_node.h`
- new `include/zerokv/core/kv_server.h`
- optional `include/zerokv/core/common_types.h` if needed for shared structs
- all includes that currently depend on `include/zerokv/kv.h`

Work:

- create new core headers first
- move `KVNode`, `KVServer`, and auxiliary KV infrastructure types out of the current `kv.h`
- decide where supporting types live:
  - node/server-facing infrastructure types go under `core`
  - top-level `KV` public surface remains in `include/zerokv/kv.h`
- keep forwarding includes or aliases temporarily so existing code still compiles during transition

Acceptance:

- `include/zerokv/kv.h` becomes the top-level `KV` header
- `KVNode` / `KVServer` are available from `include/zerokv/core/...`
- no circular include regressions

## Task 3: Namespace migration `zerokv::kv` -> `zerokv::core`

Files:

- `include/zerokv/core/*`
- `src/kv/*`
- `src/python/bindings.cpp`
- tests/examples using `zerokv::kv::*`

Work:

- rename namespace from `zerokv::kv` to `zerokv::core`
- keep temporary aliases from `zerokv::kv::*` to `zerokv::core::*` for phase 1 compatibility
- mark aliases deprecated where practical
- update Python bindings and tests/examples accordingly

Acceptance:

- internal and test code builds against `zerokv::core::*`
- compatibility alias path remains available during transition

## Task 4: Move source/header directories from `kv/` to `core/`

Files:

- `src/kv/*` -> `src/core/*`
- related CMake source lists
- include paths in code and docs

Work:

- physically move core implementation files into `src/core/`
- update CMake and include references
- keep move-only commits narrow where possible

Acceptance:

- build graph points at `src/core/*`
- no stale `src/kv/*` references remain outside temporary compatibility shims

## Task 5: Transport namespace and header move

Files:

- `include/zerokv/endpoint.h`
- `include/zerokv/worker.h`
- `include/zerokv/memory.h`
- `include/zerokv/future.h`
- `src/endpoint.cpp`
- `src/worker.cpp`
- `src/memory.cpp`
- `src/future.cpp`
- new `include/zerokv/transport/*`
- new `src/transport/*`

Work:

- move transport primitives under `zerokv::transport`
- create new header locations under `include/zerokv/transport/`
- move implementation files under `src/transport/`
- leave temporary forwarding headers at old top-level paths during phase 1 if required
- update references from `core` and `KV`

Acceptance:

- `zerokv::transport::{Endpoint,Worker,MemoryRegion,Future}` build cleanly
- `core` depends on `transport` through new includes/namespaces
- phase 1 forwarding headers preserve compatibility where needed

## Task 6: Docs, examples, bindings, and umbrella cleanup

Files:

- `README.md`
- examples
- docs under active maintenance
- `src/python/bindings.cpp`
- `include/zerokv/zerokv.h`

Work:

- rewrite user-facing docs to present `zerokv::KV` as the main API
- update examples to prefer `KV`
- adjust bindings and generated stubs if names are exposed there
- keep `Cluster` and `plugin` explicitly top-level and document that they are not part of this rename

Acceptance:

- user-facing docs no longer present `MessageKV` as the preferred API
- umbrella include exports the intended top-level surface

## Task 7: Compatibility cleanup (follow-up phase)

Files:

- deprecated aliases
- forwarding headers
- compatibility namespace shims

Work:

- remove `MessageKV` alias after downstream migration window
- remove `zerokv::kv::*` compatibility aliases
- remove old top-level transport forwarding headers if retained in phase 1

Acceptance:

- final tree contains only `KV`, `core`, and `transport` naming
- no stale compatibility layer remains

## Verification Strategy

Per task:

- build the affected targets
- run the closest focused integration/unit tests
- verify examples still compile where impacted
- perform Claude review before marking the task complete

Global verification before merge:

- full configure/build in the supported environment
- representative integration tests for `KV`, `core::KVNode`, and transport primitives
- Python bindings build verification if exposed names changed
- full grep sweep for stale top-level naming after phase completion

## Review Notes Carried Forward

From Claude spec review:

- `include/zerokv/kv.h` split must be sequenced explicitly before namespace churn
- `Cluster` and `plugin` remain top-level and are not part of this rename scope
- compatibility aliases should use explicit deprecation markers
- tests/examples referencing old namespaces should be updated in manageable batches, not a giant flag-day commit
