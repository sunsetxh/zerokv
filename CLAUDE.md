# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ZeroKV is a high-performance C++ transport library built on UCX for point-to-point data transfer (1KB-1GB) over RDMA and TCP. It targets AI distributed training, KV Cache inference transfer, and HPC scenarios.

## Build Commands

```bash
# Configure (Debug)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Configure (Release)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j$(nproc)

# Build specific target
cmake --build build --target ping_pong

# Run all tests
cd build && ctest --output-on-failure

# Run a specific test
cd build && ./tests/unit/test_config
cd build && ctest -R UnitConfig --output-on-failure
```

## Dependencies

- **Runtime**: UCX >= 1.14 (`libucx-dev` on Ubuntu/Debian, `ucx-devel` on RHEL/CentOS)
- **Build**: CMake >= 3.20, C++20 compiler (GCC >= 11 or Clang >= 14)
- **Test**: Google Test (optional, for `ZEROKV_BUILD_TESTS=ON`)
- **Benchmark**: Google Benchmark (optional, for `ZEROKV_BUILD_BENCHMARK=ON`)

## CMake Options

- `ZEROKV_BUILD_STATIC` - Build static library (default: OFF)
- `ZEROKV_BUILD_EXAMPLES` - Build examples (default: ON)
- `ZEROKV_BUILD_TESTS` - Build tests (default: ON)
- `ZEROKV_BUILD_BENCHMARK` - Build benchmarks (default: ON)
- `ZEROKV_BUILD_PYTHON` - Build Python bindings (default: ON)
- `UCX_ROOT` - Path to custom UCX installation (default: "")

## Vendored Dependencies

If `third_party/{googletest,benchmark,nanobind}` directories exist, the build
uses them via `add_subdirectory` instead of `find_package`. `third_party/ucx`
is also supported but only extends `PKG_CONFIG_PATH` (no manual find_library).

## Packaging

```bash
./scripts/package_source.sh              # with vendored deps (downloads if missing)
./scripts/package_source.sh --exclude-third-party
```

## Architecture

### Core Abstractions

The library uses a layered architecture over UCX:

```
Context (owns UCP context, global resources)
  └── Worker (per-thread progress engine, owns UCP worker)
        ├── Listener (server-side, accepts incoming connections)
        └── Endpoint (connection to a peer)
              ├── Tag-matched send/recv (two-sided)
              ├── RDMA put/get (one-sided)
              └── Stream send/recv (ordered bytes)
```

### Key Types

| Type | Purpose |
|------|---------|
| `Config` | Immutable configuration via builder pattern |
| `Context` | Top-level library handle (one per process) |
| `Worker` | Single-threaded progress engine (one per thread) |
| `Endpoint` | Connection handle for communication |
| `Listener` | Server-side connection acceptor |
| `MemoryRegion` | Registered buffer for zero-copy/RDMA |
| `Future<T>` | Async operation handle with poll/wait/callback |
| `Status` | Error handling with `ErrorCode` enum |

### Threading Model

**Thread-per-worker**: Each `Worker` is single-threaded and lock-free. Only one thread may call `progress()` or initiate operations on a given worker. For multi-threaded apps, create one worker per thread.

### Message Protocols

UCX automatically selects protocols based on message size:
- **< 8KB**: Eager (inline copy, fastest latency)
- **8KB - 256KB**: Eager zcopy
- **> 256KB**: Rendezvous (zero-copy)

## Code Style

Configured in `.clang-format`:
- LLVM base style, 4-space indent, 100-column limit
- Run `clang-format -i <file>` before committing

## Test Structure

- `tests/unit/` - Unit tests for individual components (Config, Status, Future, Memory, Cluster, KvMetadataStore, KvBench)
- `tests/integration/` - Integration tests requiring UCX runtime (Connection, TagMessaging, RDMA, KVServer, KVNode, Loopback, ClusterDiscovery)
- `tests/benchmark/` - Performance benchmarks (ping-pong latency, throughput)

Integration tests may require RDMA hardware or TCP configuration. KV
integration tests use TCP transport by default and do not require RDMA hardware.

## KV Layer

The KV module (`src/kv/`, `include/zerokv/kv.h`) provides an RDMA KV store on top
of the transport core.

### Key files

| File | Purpose |
|------|---------|
| `include/zerokv/kv.h` | Public API: KVServer, KVNode, all types |
| `src/kv/node.cpp` | KVNode implementation |
| `src/kv/server.cpp` | KVServer with subscription fan-out |
| `src/kv/protocol.h` | Wire protocol types and encode/decode |
| `src/kv/protocol.cpp` | Protocol serialization |
| `src/kv/metadata_store.h/cpp` | In-memory metadata + subscription registry |
| `src/kv/tcp_transport.h/cpp` | TCP framing utilities |
| `src/kv/tcp_framing.h/cpp` | Frame encode/decode |
| `src/kv/bench_utils.h/cpp` | Benchmark helpers |
| `examples/kv_demo.cpp` | Interactive demo |
| `examples/kv_bench.cpp` | Size-sweep benchmark |
| `examples/kv_wait_fetch.cpp` | Wait-and-fetch demo |

### KVNode operations

| Operation | Async | Description |
|-----------|-------|-------------|
| `publish` | `Future<void>` | Store key-value, become owner |
| `fetch` | `Future<FetchResult>` | RDMA get from owner |
| `fetch_to` | `Future<void>` | Zero-copy fetch into region |
| `push` | `Future<void>` | RDMA put to target node |
| `unpublish` | `Future<void>` | Remove key |
| `subscribe` | `Future<void>` | Register for key lifecycle events |
| `unsubscribe` | `Future<void>` | Stop receiving events |
| `drain_subscription_events` | sync | Poll for pending events |
| `wait_for_key` | sync | Block until key exists or timeout |
| `wait_for_keys` | sync | Batch wait, partial results on timeout |
| `subscribe_and_fetch_once` | sync | Wait + fetch single key |
| `subscribe_and_fetch_once_many` | sync | Batch wait + fetch, per-key as ready |
