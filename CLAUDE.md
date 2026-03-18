# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

AXON is a high-performance C++ transport library built on UCX for point-to-point data transfer (1KB-1GB) over RDMA and TCP. It targets AI distributed training, KV Cache inference transfer, and HPC scenarios.

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
- **Test**: Google Test (optional, for `AXON_BUILD_TESTS=ON`)
- **Benchmark**: Google Benchmark (optional, for `AXON_BUILD_BENCHMARK=ON`)

## CMake Options

- `AXON_BUILD_STATIC` - Build static library (default: OFF)
- `AXON_BUILD_EXAMPLES` - Build examples (default: ON)
- `AXON_BUILD_TESTS` - Build tests (default: ON)
- `AXON_BUILD_BENCHMARK` - Build benchmarks (default: ON)

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

- `tests/unit/` - Unit tests for individual components (Config, Status, Future, Memory)
- `tests/integration/` - Integration tests requiring UCX runtime (Connection, TagMessaging, RDMA)
- `tests/benchmark/` - Performance benchmarks (ping-pong latency, throughput)

Integration tests may require RDMA hardware or TCP configuration.
