# Implementation Plan: P2P High-Performance Transport Library MVP

**Branch**: `001-p2p-transport-mvp` | **Date**: 2026-03-04 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/001-p2p-transport-mvp/spec.md`

## Summary

Build a high-performance P2P transport library on top of UCX, providing a C++20
API for tag-matched messaging, RDMA one-sided operations, and connection
lifecycle management. The library targets AI distributed training, KV Cache
inference transfer, and HPC workloads, supporting message sizes from 0 bytes to
1GB over RDMA (InfiniBand/RoCE) and TCP transports. MVP is C++ only, host
memory only, no GPU or Python bindings.

## Technical Context

**Language/Version**: C++20 (GCC >= 11 or Clang >= 14)
**Primary Dependencies**: UCX >= 1.14 (runtime), spdlog (optional logging)
**Build System**: CMake >= 3.20
**Testing**: Google Test (unit/integration), Google Benchmark (performance)
**Target Platform**: Linux x86_64
**Project Type**: Library (shared .so + static .a)
**Performance Goals**: 1KB/4KB < 5us latency on RDMA; 1GB > 23 GB/s on 200Gbps IB; < 5% overhead vs raw UCX for messages >= 64KB
**Constraints**: Zero GPU SM consumption; thread-per-worker (no internal locking); host memory only for MVP
**Scale/Scope**: 64 concurrent connections per Worker; 0B–1GB message sizes; single-process multi-worker model

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Evidence |
|-----------|--------|----------|
| I. Requirements-Driven | ✅ PASS | Spec has 6 prioritized user stories (P1/P2), all with acceptance scenarios. Success criteria SC-001~SC-012 are quantitative with measurement methodology defined. |
| II. Architecture-First | ✅ PASS | Plan defines layered structure, UCX abstraction, thread model. Contracts defined in Phase 1. |
| III. Quality-First | ✅ PASS | C++20 with -Wall -Wextra -Werror. Status type for error handling. RAII for all UCX handles. clang-format/clang-tidy planned. |
| IV. Test-Driven | ✅ PASS | Google Test + Google Benchmark. Target 80% coverage (SC-009). Unit, integration, and performance benchmark tests planned. |
| V. Coordinated Delivery | ✅ PASS | P1 stories first (Config → Connection → Tag Messaging), then P2 (Async, Memory, RDMA). Phased delivery with checkpoints. |

**Gate result: PASS** — All 5 principles satisfied. Plan is ready.

## Project Structure

### Documentation (this feature)

```text
specs/001-p2p-transport-mvp/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output
└── tasks.md             # Phase 2 output
```

### Source Code (repository root)

```text
include/p2p/
├── common.h             # ErrorCode, Status, Tag, MemoryType
├── config.h             # Config, Config::Builder, Context
├── worker.h             # Worker, Listener
├── endpoint.h           # Endpoint (tag_send/recv, put/get/flush)
├── future.h             # Future<T>, Request
├── memory.h             # MemoryRegion
└── p2p.h               # Umbrella header

src/
├── config.cpp           # Config/Context implementation
├── worker.cpp           # Worker/Listener implementation
├── endpoint.cpp         # Endpoint implementation
├── future.cpp           # Future utilities
├── memory.cpp           # MemoryRegion implementation
├── status.cpp           # Status/error_code implementation
└── internal/
    ├── ucx_context.h    # Internal UCX context wrapper
    ├── ucx_worker.h     # Internal UCX worker wrapper
    ├── ucx_memory.h     # Internal UCX memory management
    └── ucx_utils.h      # UCX helper utilities

tests/
├── unit/
├── integration/
└── benchmark/

examples/
CMakeLists.txt
```

**Structure Decision**: Single-project library layout. Public headers in `include/p2p/`, implementation in `src/`.

## Complexity Tracking

> No constitution violations — no entries needed.
