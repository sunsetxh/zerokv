# ZeroKV

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
- **KV** — top-level distributed KV/cache API for publish/fetch/push/message-style
  workflows with bounded ack-based cleanup
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

## Logging

ZeroKV now has one unified internal logger for library diagnostics. It covers:

- `KV` sender/receiver/cleanup trace paths
- `core` control-plane and wait paths
- `transport` endpoint/worker fault paths

The logger is controlled entirely by environment variables:

```bash
export ZEROKV_LOG_LEVEL=error   # error|warn|info|debug|trace
export ZEROKV_LOG_COMPONENTS=core.kv_node,transport.endpoint
```

Behavior:

- `ZEROKV_LOG_LEVEL` defaults to `error`
- `ZEROKV_LOG_COMPONENTS` is optional
- if `ZEROKV_LOG_COMPONENTS` is unset or empty, all components are enabled
- component filters support exact match and prefix match
  - `core` enables `core.kv_node`, `core.tcp`, etc.

Current component names include:

- `kv.sender`
- `kv.receiver`
- `kv.cleanup`
- `core.kv_node`
- `core.tcp`
- `transport.endpoint`
- `transport.worker`

Examples:

```bash
# See all perf trace and fault logs
ZEROKV_LOG_LEVEL=trace ./build/message_kv_demo ...

# Only see control-plane and endpoint faults
ZEROKV_LOG_LEVEL=error \
ZEROKV_LOG_COMPONENTS=core.kv_node,transport.endpoint \
./build/message_kv_demo ...

# Only see TCP control-path faults
ZEROKV_LOG_LEVEL=error \
ZEROKV_LOG_COMPONENTS=core.tcp \
./build/message_kv_demo ...
```

Output format:

```text
[zerokv][trace][kv.sender] KV_SEND_ASYNC_ENQUEUE key=...
[zerokv][error][transport.endpoint] ucp_get_nbx failed: ...
```

Compatibility:

- existing perf-trace env vars still work during migration:
  - `ZEROKV_MESSAGE_KV_TRACE`
  - `ZEROKV_KV_TRACE`

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

## KV

`zerokv::KV` is the primary user-facing distributed KV/cache interface. It
wraps the lower-level `zerokv::core::KVNode` primitives for message-style and
object-style workflows where the application already owns unique keys.

- `send()` / `send_region()` publish message keys and record them for bounded cleanup
- `recv()` / `recv_batch()` wait for message keys and fetch into caller-owned memory
- internal ack markers stay hidden from callers

## ALPS Compatibility Wrapper

This repository also provides an ALPS-style compatibility package that keeps the
`SetClient` / `WriteBytes` / `ReadBytes` / `ReadBytesBatch` call shape while
using ZeroKV send/recv transport underneath.

Installed artifacts:

- `include/yr/alps_kv_api.h`
- `lib/libalps_kv_wrap.so`
- `bin/alps_kv_bench`
- `share/doc/alps_kv_wrap/README.md`

Build targets:

```bash
cmake --build build --target alps_kv_wrap alps_kv_bench
```

See [docs/alps_kv_wrap/README.md](docs/alps_kv_wrap/README.md)
for usage, connection modes, transport configuration, and benchmark examples.

The ALPS wrapper benchmark defaults to a medium/large payload sweep:
`256K,512K,1M,2M,4M,8M,16M,32M,64M`.

### Transport selection (ALPS wrapper)

The transport is controlled entirely by environment variables; no rebuild is
needed to switch between TCP and RDMA:

| Variable | Example | Purpose |
|---|---|---|
| `UCX_NET_DEVICES` | `rocep23s0f0:1` | Restrict to a specific NIC port |
| `UCX_TLS` | `rc,sm,self` | UCX transport list (RC = RoCE, tcp = TCP) |
| `ZEROKV_TRANSPORT` | `rdma` | High-level preset (`rdma` → `rc,sm,self`) |

**RoCE quick start:**

```bash
# Server
UCX_NET_DEVICES=rocep23s0f0:1 UCX_TLS=rc,sm,self \
./alps_kv_bench --mode server --port 16000

# Client
UCX_NET_DEVICES=rocep23s0f0:1 UCX_TLS=rc,sm,self \
./alps_kv_bench --mode client --host <server_ip> --port 16000
```

> **Note**: setting `UCX_NET_DEVICES` to a RoCE/IB device without specifying
> `UCX_TLS` (or with `UCX_TLS=tcp`) causes UCX to fail with
> "no able transports/devices". Always pair a RoCE device with an RDMA TLS such
> as `rc,sm,self`.

### KV two-node demo

`message_kv_demo` sweeps message sizes with `--sizes` across the two-node
scenario. The default sweep list is `1K,64K,1M,4M,16M,32M,64M,128M`.

- `RANK0` runs `KVServer + KV receiver` in one process
- `RANK1` sends one message per round from 4 threads, reusing a preallocated send buffer per thread
- `RANK1` supports `--send-mode sync|async`; `sync` keeps the old blocking ack
  semantics, `async` measures sender-side publish/enqueue cost while still
  waiting for all futures to complete before ending each round
- both sides default to `--warmup-rounds 1`, reusing `KV` instances across
  rounds so the measured rounds are closer to steady-state behavior
- `RANK0` preallocates one receive region sized to `messages * max(--sizes)` and
  reuses it across measured rounds; `RANK1` does the same for per-thread send buffers
- the last round still needs about `4 * 128MiB = 512MiB` of receive payload space
- the warmup rounds use the first size in `--sizes` and do not print
  `SEND_ROUND` / `RECV_ROUND`
- `--post-recv-wait-ms` is applied once after the measured sweep completes; it is
  not inserted between measured rounds
- if you want raw cold-start behavior, set `--warmup-rounds 0`

Build:

```bash
cmake -S . -B build \
  -DUCX_ROOT=/opt/ucx \
  -DZEROKV_BUILD_TESTS=ON \
  -DZEROKV_BUILD_BENCHMARK=ON \
  -DZEROKV_BUILD_PYTHON=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)" --target message_kv_demo kv_demo
```

Run on two nodes:

```bash
# RANK0: colocated server + receiver in one process
./build/message_kv_demo \
  --role rank0 \
  --listen 10.0.0.1:15000 \
  --data-addr 10.0.0.1:0 \
  --node-id rank0-receiver \
  --messages 4 \
  --warmup-rounds 1 \
  --sizes 1K,64K,1M,4M,16M,32M,64M,128M \
  --timeout-ms 30000 \
  --transport rdma

# RANK1: sender with 4 worker threads
./build/message_kv_demo \
  --role rank1 \
  --server-addr 10.0.0.1:15000 \
  --data-addr 10.0.0.2:0 \
  --node-id rank1-sender \
  --threads 4 \
  --send-mode async \
  --warmup-rounds 1 \
  --sizes 1K,64K,1M,4M,16M,32M,64M,128M \
  --transport rdma
```

Expected receiver output per round:

```text
RECV_ROUND round=0 size=1024 completed=4 failed=0 timed_out=0 completed_all=1 recv_total_us=... total_bytes=4096 throughput_MiBps=...
RECV_OK key=msg-round0-size1024-thread0 bytes=1024 preview=round0-thread0-xxxxxxxxx...
RECV_OK key=msg-round0-size1024-thread1 bytes=1024 preview=round0-thread1-xxxxxxxxx...
RECV_OK key=msg-round0-size1024-thread2 bytes=1024 preview=round0-thread2-xxxxxxxxx...
RECV_OK key=msg-round0-size1024-thread3 bytes=1024 preview=round0-thread3-xxxxxxxxx...
```

Expected sender output per round also includes per-thread send latency and one
aggregate summary:

```text
SEND_OK key=msg-round0-size1024-thread0 bytes=1024 send_mode=async send_us=...
...
SEND_ROUND round=0 send_mode=async size=1024 messages=4 send_total_us=... max_thread_send_us=... total_bytes=4096 throughput_MiBps=...
```

Implementation note:

- `KV` Phase 1 serializes public calls inside one wrapper instance.
- To stay close to the real multi-threaded sender pattern, the demo gives each
  sender thread its own `KV` instance and node id.
- `send_region()` is synchronous with receiver acknowledgement:
  the sender publishes the message key, waits for the receiver's internal ack
  key, then unpublishes the message key before returning.
- `send_region_async()` returns earlier, but each future completes only after
  the receiver ack arrives and the sender has removed the message metadata.

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
