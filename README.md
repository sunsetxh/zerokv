# AXON

High-performance C++ transport library built on UCX for point-to-point data
transfer (1 KB - 1 GB) over RDMA and TCP. Targeting AI distributed training,
KV cache inference transfer, and HPC scenarios.

## Features

- **Transport core** — tag-matched send/recv, RDMA put/get, stream I/O over UCX
- **RDMA KV store** — server-mediated metadata with client-to-client zero-copy
  data transfer
- **Operations** — publish, fetch, push, unpublish, subscribe
- **Metrics** — per-operation latency breakdown (publish, fetch, push)
- **Benchmark** — size-sweep publish/fetch benchmark (`kv_bench`)

## Prerequisites

| Dependency | Version | Notes |
|---|---|---|
| C++ compiler | GCC >= 11 / Clang >= 14 | C++20 required |
| CMake | >= 3.20 | |
| UCX | >= 1.14 | `libucx-dev` (Debian) or `ucx-devel` (RHEL) |
| Google Test | optional | for `AXON_BUILD_TESTS=ON` |
| Google Benchmark | optional | for `AXON_BUILD_BENCHMARK=ON` |
| nanobind | optional | for `AXON_BUILD_PYTHON=ON` |

## Build

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Point to a custom UCX installation
cmake -B build -DCMAKE_BUILD_TYPE=Release -DUCX_ROOT=/opt/ucx

# Build
cmake --build build -j$(nproc)

# Test
cd build && ctest --output-on-failure
```

### Vendored dependencies

Place source trees under `third_party/` to build without system packages:

```
third_party/
  googletest/    # Google Test
  benchmark/     # Google Benchmark
  nanobind/      # nanobind
  ucx/           # UCX (optional)
```

The build prefers vendored sources over system `find_package`. If a dependency
is missing from both locations, that feature is disabled automatically.

### Packaging

```bash
# Create source tarball with vendored dependencies
./scripts/package_source.sh

# Without vendored dependencies
./scripts/package_source.sh --exclude-third-party
```

By default the packaging script copies the repository root and includes vendored
dependency source trees. If `third_party/{googletest,benchmark,nanobind,ucx}`
is missing locally, the script downloads the matching release tarballs before
creating the archive.

## CMake Options

| Option | Default | Description |
|---|---|---|
| `UCX_ROOT` | "" | Path to custom UCX installation |
| `AXON_BUILD_STATIC` | OFF | Build static library |
| `AXON_BUILD_EXAMPLES` | ON | Build examples |
| `AXON_BUILD_TESTS` | ON | Build tests |
| `AXON_BUILD_BENCHMARK` | ON | Build benchmarks |
| `AXON_BUILD_PYTHON` | ON | Build Python bindings |

## Quick Start

### Transport examples

```bash
# In terminal 1
./build/ping_pong --mode server --listen 0.0.0.0:5000

# In terminal 2
./build/ping_pong --mode client --connect <server_ip>:5000 --transport rdma
```

### KV demo

```bash
# Server
./build/kv_demo --mode server --listen 0.0.0.0:15000 --transport rdma

# Publisher
./build/kv_demo --mode publish --server-addr <server_ip>:15000 \
  --data-addr 0.0.0.0:0 --node-id pub1 --key mykey \
  --value hello-rdma --transport rdma --hold

# Fetcher (from another machine)
./build/kv_demo --mode fetch --server-addr <server_ip>:15000 \
  --data-addr 0.0.0.0:0 --node-id reader1 --key mykey \
  --transport rdma
```

### KV benchmark

```bash
# Server
./build/kv_bench --mode server --listen 0.0.0.0:15000 --transport rdma

# Stable owner (for fetch benchmark)
./build/kv_bench --mode hold-owner --server-addr <server_ip>:15000 \
  --data-addr 0.0.0.0:0 --node-id owner --transport rdma

# Publish benchmark
./build/kv_bench --mode bench-publish --server-addr <server_ip>:15000 \
  --data-addr 0.0.0.0:0 --node-id bp1 --sizes 4K,64K,1M,16M,128M \
  --total-bytes 1G --transport rdma

# Fetch benchmark
./build/kv_bench --mode bench-fetch --server-addr <server_ip>:15000 \
  --data-addr 0.0.0.0:0 --node-id bf1 --owner-node-id owner \
  --sizes 4K,64K,1M,16M,128M --total-bytes 1G --transport rdma
```

## Architecture

```
Context (UCP context, global resources)
  └── Worker (per-thread progress engine)
        ├── Listener (server-side acceptor)
        └── Endpoint (peer connection)
              ├── Tag send/recv (two-sided)
              ├── RDMA put/get (one-sided)
              └── Stream send/recv (ordered bytes)
```

### KV layer (on top of transport)

```
KVServer (metadata directory, subscription fan-out)
KVNode   (data owner, publish/fetch/push/subscribe)
  ├── Control plane: TCP to server (register, lookup, subscribe)
  ├── Data plane:    RDMA to peers (zero-copy get/put)
  └── Push plane:    RDMA put + direct TCP commit to target
```

## Soft-RoCE / QEMU Environment

When using Soft-RoCE (`rxe0`) in VMs, you may need:

```bash
export UCX_PROTO_ENABLE=n
export UCX_NET_DEVICES=rxe0:1
```

## License

See LICENSE file for details.
