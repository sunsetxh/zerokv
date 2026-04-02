# KV / Core / Transport Rename and Layering Design

## Goal

Reorganize ZeroKV into three explicit layers:

- `zerokv::KV`: primary user-facing distributed KV/cache interface
- `zerokv::core::*`: metadata, control-plane, and KV orchestration internals
- `zerokv::transport::*`: low-level transport primitives and UCX-backed communication building blocks

This change is not only a rename. It is a structural cleanup to correct the current layering drift where:

- `MessageKV` has evolved into the main user-facing distributed KV/cache API
- `KVNode`/`KVServer` are infrastructure rather than the preferred top-level API
- `Endpoint`/`Worker`/`MemoryRegion`/`Future` are transport primitives but currently live alongside product-facing interfaces

## Desired Public Shape

Top-level public API should converge to:

- `zerokv::KV`
- `zerokv::Config`
- `zerokv::zerokv.h` as umbrella include

Top-level should stop exposing infrastructure-heavy naming as the primary entrypoint.

## Layering Model

### 1. Top-Level User API

Purpose:

- the default API most users should instantiate directly
- distributed KV/cache semantics for publish/fetch/ack-style workflows
- current `MessageKV` behavior and performance path become the foundation of this layer

Target:

- `include/zerokv/kv.h` declares `zerokv::KV`
- `src/kv.cpp` or equivalent top-level implementation file backs `zerokv::KV`
- `include/zerokv/zerokv.h` includes top-level user API headers

Rules:

- top-level should prefer stable user-facing concepts
- top-level should not directly expose metadata server mechanics unless intentionally advanced
- `Config` remains in `zerokv::Config` at the top level

### 2. Core Layer

Purpose:

- metadata registration
- subscription and control-plane orchestration
- key publication lifecycle
- server/node coordination
- internal protocol and metadata store responsibilities

Target namespace:

- `zerokv::core`

Target files:

- `include/zerokv/core/kv_node.h`
- `include/zerokv/core/kv_server.h`
- optionally `include/zerokv/core/protocol.h` if external visibility remains necessary
- `src/core/node.cpp`
- `src/core/server.cpp`
- `src/core/protocol.*`
- `src/core/metadata_store.*`
- `src/core/tcp_framing.*`
- `src/core/tcp_transport.*`
- related bench utilities move under `src/core/` if still tied to this layer

Rules:

- `KVNode` and `KVServer` remain available, but as infrastructure APIs
- `core` depends on `transport`
- `core` does not become the default product-facing namespace

### 3. Transport Layer

Purpose:

- UCX-backed communication primitives
- endpoint creation and message/RMA operations
- worker progress and connection management
- memory registration and remote keys
- request/future wrappers and transport completion tracking

Target namespace:

- `zerokv::transport`

Target files:

- `include/zerokv/transport/endpoint.h`
- `include/zerokv/transport/worker.h`
- `include/zerokv/transport/memory.h`
- `include/zerokv/transport/future.h`
- `src/transport/endpoint.cpp`
- `src/transport/worker.cpp`
- `src/transport/memory.cpp`
- `src/transport/future.cpp`
- existing UCX support files remain under `src/internal/` or may move later if worth it

Rules:

- transport should contain no KV-specific concepts
- transport is the lowest public abstraction layer
- transport can remain advanced-user-facing, but not the default on-ramp

## Naming Changes

### Top-Level Rename

- `zerokv::MessageKV` -> `zerokv::KV`
- `include/zerokv/message_kv.h` -> retired or replaced by `include/zerokv/kv.h`
- implementation file renamed accordingly

### Core Rename

- current `namespace zerokv::kv` -> `namespace zerokv::core`
- `KVNode` / `KVServer` move under `core`

### Transport Rename

- `Endpoint`, `Worker`, `MemoryRegion`, `Future`, and related request helpers move under `zerokv::transport`
- include paths move accordingly

## Compatibility Strategy

This rename is a deliberate API cleanup, but it should be staged to avoid unnecessary disruption.

### Phase 1: Structural Move with Compatibility Aliases

Introduce the new layout while preserving old includes and aliases temporarily:

- `zerokv::KV` introduced as the primary type
- old `zerokv::MessageKV` may remain as a deprecated alias during the transition window
- `zerokv::kv::*` may remain as deprecated aliases to `zerokv::core::*`
- old transport headers may forward to `zerokv/transport/*` temporarily if needed

### Phase 2: Remove Transitional Surface

After migration lands and downstream users are updated:

- remove deprecated aliases and forwarding headers
- keep only the new layout

## Directory Reshape

Target shape:

- `include/zerokv/kv.h`
- `include/zerokv/config.h`
- `include/zerokv/zerokv.h`
- `include/zerokv/core/...`
- `include/zerokv/transport/...`
- `src/kv.cpp`
- `src/core/...`
- `src/transport/...`

This intentionally separates:

- product-level API
- orchestration/core logic
- transport primitives

## Documentation Impact

Update all active docs and examples to reflect the new layering:

- README examples should use `zerokv::KV`
- docs should describe `core` and `transport` as lower layers
- performance notes should refer to `KV` rather than `MessageKV`
- any remaining "message_kv" narrative should be rewritten to "KV" or "distributed KV/cache" wording

## Testing Requirements

At minimum:

1. build succeeds after directory and namespace moves
2. existing `MessageKV` integration coverage is preserved under `KV`
3. `KVNode` / `KVServer` tests continue to pass under `zerokv::core`
4. transport-layer tests and public examples compile under `zerokv::transport`
5. umbrella include `zerokv/zerokv.h` exports the intended top-level API cleanly
6. transitional compatibility aliases, if kept in phase 1, have explicit tests

## Risks

### 1. Rename churn

A large set of includes, namespaces, and docs will move together. This is manageable but should be staged.

### 2. Ambiguous ownership of files during migration

The current tree mixes layers. The implementation plan must split the work into narrow rename/move batches to avoid breaking everything at once.

### 3. Overexposing transport details

During the move, it is easy to accidentally keep transport headers in the top-level umbrella. The umbrella should stay intentionally minimal.

### 4. Partial rename confusion

Doing only the type rename without directory and namespace cleanup would worsen confusion. The work should explicitly include directory restructuring.

## Recommendation

Proceed with a staged migration in this order:

1. introduce top-level `KV` and keep temporary compatibility with `MessageKV`
2. move `zerokv::kv` to `zerokv::core`
3. move transport primitives to `zerokv::transport`
4. update docs/examples/umbrella includes
5. remove compatibility shims in a follow-up cleanup once downstream migration is complete
