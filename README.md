# AXON

High-performance C++ transport library built on UCX for point-to-point data
transfer (1 KB - 1 GB) over RDMA and TCP. Targeting AI distributed training,
KV cache inference transfer, and HPC scenarios.

## Features

- **Transport core** — tag-matched send/recv, RDMA put/get, stream I/O over UCX
- **RDMA KV store** — server-mediated metadata with client-to-client zero-copy
  data transfer
- **Operations** — publish, fetch, push, unpublish, subscribe
- **Subscription** — best-effort key lifecycle events (published, updated,
  unpublished, owner lost) with polling API
- **Wait-and-fetch** — synchronous helpers to wait for keys then fetch them:
  single-key and batch, partial results on timeout, first-success-wins
- **Metrics** — per-operation latency breakdown (publish, fetch, push)
- **Benchmark** — size-sweep publish/fetch benchmark (`kv_bench`)
- **Python bindings** — nanobind-based Python API for KV operations
- **Vendor build** — optional vendored dependencies under `third_party/`,
  offline-capable source packaging

## Prerequisites

| Dependency | Version | Notes |
|---|---|---|
| C++ compiler | GCC >= 11 / Clang >= 14 | C++20 required |
| CMake | >= 3.20 | |
| UCX | >= 1.14 | `libucx-dev` (Debian) or `ucx-devel` (RHEL) |
| Google Test | optional | for `ZEROKV_BUILD_TESTS=ON` |
| Google Benchmark | optional | for `ZEROKV_BUILD_BENCHMARK=ON` |
| nanobind | optional | for `ZEROKV_BUILD_PYTHON=ON` |

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
| `ZEROKV_BUILD_STATIC` | OFF | Build static library |
| `ZEROKV_BUILD_EXAMPLES` | ON | Build examples |
| `ZEROKV_BUILD_TESTS` | ON | Build tests |
| `ZEROKV_BUILD_BENCHMARK` | ON | Build benchmarks |
| `ZEROKV_BUILD_PYTHON` | ON | Build Python bindings |

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

# Stable owner (only needed for fetch benchmark)
./build/kv_bench --mode hold-owner --server-addr <server_ip>:15000 \
  --data-addr 0.0.0.0:0 --node-id owner --transport rdma

# Publish benchmark
# The benchmark node itself is the owner for its temporary published keys.
./build/kv_bench --mode bench-publish --server-addr <server_ip>:15000 \
  --data-addr 0.0.0.0:0 --node-id bp1 --publish-api copy \
  --sizes 4K,64K,1M,16M,128M \
  --iters 4 --transport rdma

# Zero-copy publish benchmark
./build/kv_bench --mode bench-publish --server-addr <server_ip>:15000 \
  --data-addr 0.0.0.0:0 --node-id bp1z --publish-api region \
  --iters 4 --transport rdma

# Fetch benchmark
./build/kv_bench --mode bench-fetch --server-addr <server_ip>:15000 \
  --data-addr 0.0.0.0:0 --node-id bf1 \
  --sizes 4K,64K,1M,16M,128M --iters 4 --transport rdma

# Zero-copy fetch benchmark
./build/kv_bench --mode bench-fetch-to --server-addr <server_ip>:15000 \
  --data-addr 0.0.0.0:0 --node-id bf1z \
  --sizes 4K,64K,1M,16M,128M --iters 4 --transport rdma
```

`--total-bytes` is still supported, but fixed `--iters` is the recommended
first pass when validating across real machines.

`fetch()` is the convenience end-to-end API that returns owned bytes.
`fetch_to()` is the zero-copy public API for performance-sensitive paths.
Accordingly:

- `bench-fetch` measures end-to-end fetch cost and includes the final result copy
- `bench-fetch-to` measures the zero-copy path more directly
- `--owner-node-id` is optional; when provided, it acts as a topology sanity
  check against the owner returned by metadata
- `bench-publish` supports `--publish-api copy|region`
  - `copy` uses `publish()`
  - `region` uses `publish_region()` and is the zero-copy publish path

### Python KV example

After building with `-DZEROKV_BUILD_PYTHON=ON`, you can smoke-test the modern
Python API with the bundled example:

```bash
# Terminal 1: server
python3 python/examples/kv_example.py \
  --mode server \
  --listen 0.0.0.0:15000 \
  --transport rdma

# Terminal 2: publisher
python3 python/examples/kv_example.py \
  --mode publish \
  --server-addr <server_ip>:15000 \
  --data-addr 0.0.0.0:0 \
  --node-id py-publisher \
  --key demo-key \
  --value hello-from-python \
  --transport rdma \
  --hold

# Terminal 3: fetcher
python3 python/examples/kv_example.py \
  --mode fetch \
  --server-addr <server_ip>:15000 \
  --data-addr 0.0.0.0:0 \
  --node-id py-reader \
  --key demo-key \
  --transport rdma
```

The example prints the fetched payload and the latest publish/fetch metrics.

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
KVNode   (data owner, publish/fetch/push/subscribe/wait-and-fetch)
  ├── Control plane: TCP to server (register, lookup, subscribe)
  ├── Data plane:    RDMA to peers (zero-copy get/put)
  ├── Push plane:    RDMA put + direct TCP commit to target
  └── Subscription:  dedicated TCP listener for event delivery
```

### KV API overview

```cpp
// Server
server->start({"0.0.0.0:15000"});
server->lookup("my-key");   // -> optional<KeyInfo>
server->stop();

// Node
node->start({"server:15000", "0.0.0.0:0", "node-1"});
node->publish("key", data, size);
node->fetch("key");                      // -> Future<FetchResult>
node->push("target-node", "key", data, size);
node->unpublish("key");
node->subscribe("key");
node->unsubscribe("key");
node->drain_subscription_events();       // -> vector<SubscriptionEvent>

// Wait-and-fetch helpers (synchronous)
node->wait_for_key("key", timeout);      // -> Status
node->wait_for_keys({"a","b"}, timeout); // -> WaitKeysResult
node->subscribe_and_fetch_once("key", timeout);       // -> FetchResult
node->subscribe_and_fetch_once_many({"a","b"}, timeout); // -> BatchFetchResult
```

### Wait-And-Fetch Example

`kv_wait_fetch` demonstrates the "subscribe before key exists, then fetch when it
appears" workflow.

```bash
# Terminal 1: server
./kv_demo --mode server --listen 10.0.0.1:15150 --transport rdma

# Terminal 2: waiter
UCX_PROTO_ENABLE=n UCX_NET_DEVICES=rxe0:1 ./kv_wait_fetch \
  --mode subscribe-fetch-once \
  --server-addr 10.0.0.1:15150 \
  --data-addr 10.0.0.1:0 \
  --node-id waiter \
  --key waitfetch-key \
  --transport rdma

# Terminal 3: publisher
UCX_PROTO_ENABLE=n UCX_NET_DEVICES=rxe0:1 ./kv_demo \
  --mode publish \
  --server-addr 10.0.0.1:15150 \
  --data-addr 10.0.0.2:0 \
  --node-id publisher \
  --key waitfetch-key \
  --value hello-waitfetch \
  --transport rdma \
  --hold
```

Expected waiter output:

```text
FETCH_OK key=waitfetch-key owner=publisher version=1 value=hello-waitfetch
```

## Soft-RoCE / QEMU Environment

When using Soft-RoCE (`rxe0`) in VMs, you may need:

```bash
export UCX_PROTO_ENABLE=n
export UCX_NET_DEVICES=rxe0:1
```

## License

See LICENSE file for details.
