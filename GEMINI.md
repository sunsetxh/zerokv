# GEMINI.md - AXON Transport Library Context

This file provides architectural overview, development standards, and operational guidance for the `axon` project.

## Project Overview

`axon` is a high-performance C++ transport library built on **OpenUCX (UCP layer)**. It provides low-latency, high-bandwidth point-to-point data transfer (1KB-1GB) over **RDMA (InfiniBand/RoCE)** and **TCP**.

### Core Use Cases
- **AI Distributed Training**: Gradient synchronization and parameter exchange.
- **Inference Transfer**: Low-latency KV Cache transfer between nodes.
- **HPC Scenarios**: General-purpose high-performance message passing.

### Technologies
- **Language**: C++20 (primary), Python 3.8+ (via `nanobind`).
- **Backend**: OpenUCX >= 1.14.
- **Build System**: CMake >= 3.20.
- **Testing/Benchmarking**: Google Test, Google Benchmark.

---

## Architecture & Key Concepts

The library follows a layered architecture over UCX, emphasizing a **lock-free, thread-per-worker** model.

### Key Entities
| Entity | Scope | Description |
| :--- | :--- | :--- |
| **Context** | Process | Owns global resources and the underlying UCP context. Created from a `Config`. |
| **Worker** | Thread | Progress engine for a specific thread. Drives async completions via `progress()`. |
| **Endpoint** | Connection | Logical link to a remote peer. Supports Tag Messaging and RDMA. |
| **Listener** | Server | Accepts incoming connections on a bound address. |
| **MemoryRegion** | Buffer | Registered memory for zero-copy transfers and RDMA remote access. |
| **Future<T>** | Operation | Handle for async operations; supports `ready()`, `wait()`, and `then()`. |

### Threading Model: Thread-per-Worker
- Each `Worker` is **single-threaded** and **not thread-safe**.
- For multi-threaded applications, create one `Worker` per thread.
- Only the thread owning the `Worker` should call `progress()` or initiate operations on its associated `Endpoints`.

---

## Building and Running

### Prerequisites
- **Ubuntu/Debian**: `sudo apt install libucx-dev cmake g++-11`
- **RHEL/CentOS**: `sudo yum install ucx-devel cmake gcc-c++`

### Build Commands
```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DAXON_BUILD_TESTS=ON \
      -DAXON_BUILD_EXAMPLES=ON \
      -DAXON_BUILD_PYTHON=ON

# Build all targets
cmake --build build -j$(nproc)

# Build specific example
cmake --build build --target ping_pong
```

### Running Tests & Benchmarks
```bash
# Run all tests
cd build && ctest --output-on-failure

# Run specific unit test
./build/tests/unit/test_config

# Run benchmark
./build/tests/benchmark/bench_pingpong
```

---

## Development Conventions

### Code Style
- **Standard**: C++20.
- **Formatting**: LLVM-based style, 4-space indent, 100-column limit.
- **Tool**: Use `clang-format -i <file>` (configured via `.clang-format`).

### Error Handling
- Functions return `axon::Status`.
- Use `status.ok()` to check success.
- Use `status.throw_if_error()` to convert to `AXONError` exception (common in Python/top-level C++).

### Testing Standards
- **Unit Tests**: Place in `tests/unit/`. Focus on logic without network side effects (e.g., `Config`, `Status`).
- **Integration Tests**: Place in `tests/integration/`. Require UCX runtime and potentially network hardware.
- **Benchmarks**: Place in `tests/benchmark/`. Use Google Benchmark for latency/throughput metrics.

### Python Bindings
- Located in `python/axon/`.
- Built using `nanobind`.
- The native module is `_core`, exposed through the `axon` package.

---

## Directory Structure
- `include/axon/`: Public C++ headers.
- `src/`: Core implementation.
  - `src/core/`: Internal logic.
  - `src/transport/`: UCX abstraction and backend management.
  - `src/plugin/`: Collective offload plugins (NCCL, HCCL).
- `python/`: Python package and bindings code.
- `specs/`: Detailed design specifications and feature plans.
- `docs/`: Architectural reports, risk assessments, and manuals.
- `examples/`: Usage samples for C++ and Python.
