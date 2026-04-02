# ZeroKV Current Implementation Summary

## Overview

The current `zerokv` implementation is a UCX/UCP-based asynchronous communication layer.
Its core stack is:

`Config/Context -> Worker/Listener -> Endpoint -> MemoryRegion/RemoteKey -> Request/Future`

This is already sufficient to support:

- connection establishment
- asynchronous progress
- tag messaging
- stream messaging
- one-sided RDMA put/get
- remote key exchange for RMA

It is not yet a fully closed general-purpose runtime. Some APIs are present but only partially implemented, and several advanced pieces remain placeholders.

## Architecture

### 1. Config and Context

Files:

- [include/zerokv/config.h](/Users/wangyuchao/code/axon/include/axon/config.h)
- [src/config.cpp](/Users/wangyuchao/code/axon/src/config.cpp)

Responsibilities:

- build immutable runtime configuration
- expose transport selection such as `tcp`, `shmem`, `rdma`, `rdma_ud`
- pass UCX options through `Config::Builder::set()`
- create the top-level UCX `ucp_context_h`

Current UCX features requested during context creation:

- `UCP_FEATURE_TAG`
- `UCP_FEATURE_STREAM`
- `UCP_FEATURE_RMA`
- `UCP_FEATURE_AMO64`

Current transport mapping:

- `tcp` -> `UCX_TLS=tcp`
- `shmem` -> `UCX_TLS=sm`
- `rdma` -> `UCX_TLS=rc,sm,self`
- `rdma_ud` -> `UCX_TLS=ud,sm,self`

### 2. Worker and Listener

Files:

- [include/zerokv/worker.h](/Users/wangyuchao/code/axon/include/axon/worker.h)
- [src/worker.cpp](/Users/wangyuchao/code/axon/src/worker.cpp)

Responsibilities:

- create and own one `ucp_worker_h`
- drive UCX progress
- create outgoing connections
- create listeners for incoming connections
- optionally run a background progress thread

Current connection models:

- worker-address based connection using exported UCX worker address blobs
- sockaddr-based client/server connection using `host:port`

Listener behavior today:

- `Worker::listen()` creates a UCX listener
- the UCX connection callback immediately creates the server-side `Endpoint`
- the accepted endpoint is delivered through `AcceptCallback`

### 3. Endpoint

Files:

- [include/zerokv/endpoint.h](/Users/wangyuchao/code/axon/include/axon/endpoint.h)
- [src/endpoint.cpp](/Users/wangyuchao/code/axon/src/endpoint.cpp)

Responsibilities:

- two-sided communication
- one-sided RDMA operations
- stream operations
- endpoint flush and close
- bridge UCX requests into `Future<T>`

API groups:

- tag send/recv
- stream send/recv
- RDMA `put`
- RDMA `get`
- atomic `fadd`
- atomic `cswap`
- `flush`
- `close`

### 4. Memory and Remote Key Handling

Files:

- [include/zerokv/memory.h](/Users/wangyuchao/code/axon/include/axon/memory.h)
- [src/memory.cpp](/Users/wangyuchao/code/axon/src/memory.cpp)

Responsibilities:

- register user memory with UCX
- allocate and register managed buffers
- export packed remote keys
- provide the local memory object used by put/get

Current data flow for RMA:

1. server allocates or registers memory
2. server packs `RemoteKey`
3. server sends packed key and remote virtual address to peer
4. client unpacks key with `ucp_ep_rkey_unpack`
5. client issues `put` or `get`

### 5. Request and Future

Files:

- [include/zerokv/future.h](/Users/wangyuchao/code/axon/include/axon/future.h)
- [src/future.cpp](/Users/wangyuchao/code/axon/src/future.cpp)

Responsibilities:

- represent one in-flight UCX request
- expose status polling and blocking wait
- keep async resources alive until completion
- provide typed result extraction for common operations

Current typed future coverage:

- `Future<void>` for send, put, get, flush, close
- `Future<size_t>` for stream receive
- `Future<std::pair<size_t, Tag>>` for tag receive
- `Future<uint64_t>` for atomic operations

## Supported Functionality

### Connection and Progress

Implemented:

- `Context::create()`
- `Worker::create()`
- `Worker::connect(const std::vector<uint8_t>&)`
- `Worker::connect(const std::string&)`
- `Worker::listen()`
- `Worker::progress()`
- `Worker::wait()`
- `Worker::run()`
- background progress thread start/stop

Notes:

- sockaddr connection currently assumes IPv4 numeric addresses
- worker progress is functional both manually and in a background thread

### Two-Sided Communication

Implemented:

- `Endpoint::tag_send(const void*, size_t, Tag)`
- `Endpoint::tag_send(const MemoryRegion::Ptr&, size_t, size_t, Tag)`
- `Endpoint::tag_recv(void*, size_t, Tag, TagMask)`
- `Worker::tag_recv(void*, size_t, Tag, TagMask)`
- `Endpoint::stream_send(...)`
- `Endpoint::stream_recv(...)`

### One-Sided RDMA

Implemented:

- `MemoryRegion::remote_key()`
- `Endpoint::put(...)`
- `Endpoint::get(...)`
- `Endpoint::flush()`

Verified in current QEMU Soft-RoCE environment:

- remote key exchange
- RDMA put
- RDMA get
- multi-size put/get performance runs across two VMs

### Atomic Operations

API exists:

- `Endpoint::atomic_fadd(...)`
- `Endpoint::atomic_cswap(...)`

Current status:

- code path exists in `src/endpoint.cpp`
- not validated as working in the current Soft-RoCE environment
- should be treated as experimental

### Higher-Level Surface Area

Present in the repository:

- umbrella header [include/zerokv/axon.h](/Users/wangyuchao/code/axon/include/axon/axon.h)
- cluster layer [include/zerokv/cluster.h](/Users/wangyuchao/code/axon/include/axon/cluster.h)
- Python bindings [src/python/bindings.cpp](/Users/wangyuchao/code/axon/src/python/bindings.cpp)
- plugin integrations under [src/plugin](/Users/wangyuchao/code/axon/src/plugin)

### MessageKV Steady-State Notes

Recent MessageKV tuning confirmed two practical points for the current large-message
path:

- `send_region()` plus preallocated send buffers is the intended steady-state sender path
- the two-node `message_kv_demo` should also reuse one receive region across
  measured rounds on `RANK0`

That second point matters because re-registering a large receive region every
round can dominate the sender-observed latency even when the receiver's
`recv_batch()` itself is already fast. In real-environment measurements, a
per-round receive-region allocation on `RANK0` produced an artificial fixed
delay of about 2 seconds for `1MiB+` sender rounds. Reusing the receive region
across rounds removed that false bottleneck.

After the fix, the latest real-environment `message_kv_demo` numbers show:

| size | sender throughput (`SEND_ROUND`) | receiver throughput (`RECV_ROUND`) |
|---|---:|---:|
| 64K | 201.776 MiB/s | 211.685 MiB/s |
| 1M | 982.56 MiB/s | 3189.79 MiB/s |
| 4M | 1634.32 MiB/s | 5710.21 MiB/s |
| 16M | 3204.97 MiB/s | 8116.68 MiB/s |
| 32M | 3607.56 MiB/s | 8607.36 MiB/s |
| 64M | 3966.05 MiB/s | 8932.62 MiB/s |
| 128M | 3851.74 MiB/s | 9183.2 MiB/s |

Interpretation:

- the receiver side is no longer showing an obvious steady-state anomaly for `1MiB+`
- the sender side is still slower than pure `fetch_to` benchmarks because
  `send_region()` remains synchronous with ack and cleanup semantics
- current large-payload optimization work should therefore focus more on sender
  control-path cost than on receiver data-path correctness

## Known Gaps and Unimplemented Items

### Public API Present but Not Implemented

1. `tag_recv` into `MemoryRegion`

Files:

- [src/endpoint.cpp](/Users/wangyuchao/code/axon/src/endpoint.cpp)
- [src/worker.cpp](/Users/wangyuchao/code/axon/src/worker.cpp)

Current behavior:

- returns `kNotImplemented`

2. `Future::then(...)`

File:

- [include/zerokv/future.h](/Users/wangyuchao/code/axon/include/axon/future.h)

Current behavior:

- placeholder returning `kNotImplemented`

3. `Future::on_complete(...)`

File:

- [include/zerokv/future.h](/Users/wangyuchao/code/axon/include/axon/future.h)

Current behavior:

- declared but currently empty

### Declared Design but Missing or Incomplete Backing Implementation

1. `MemoryPool`
2. `RegistrationCache`

File:

- [include/zerokv/memory.h](/Users/wangyuchao/code/axon/include/axon/memory.h)

Current status:

- documented in the public API
- no corresponding complete implementation found in the current source set

### Partially Implemented or Semantically Weak Areas

1. `Endpoint::remote_address()`

File:

- [src/endpoint.cpp](/Users/wangyuchao/code/axon/src/endpoint.cpp)

Current behavior:

- always returns an empty string

2. `Endpoint::set_error_callback(...)`

File:

- [src/endpoint.cpp](/Users/wangyuchao/code/axon/src/endpoint.cpp)

Current behavior:

- stores the callback
- no clear error-handler wiring to UCX endpoint error events yet

3. `Listener::accept()`

Files:

- [include/zerokv/worker.h](/Users/wangyuchao/code/axon/include/axon/worker.h)
- [src/worker.cpp](/Users/wangyuchao/code/axon/src/worker.cpp)

Current behavior:

- connection acceptance is callback-driven in `connection_handler`
- the polling-style `accept()` path is no longer the real mechanism

4. sockaddr parsing

File:

- [src/worker.cpp](/Users/wangyuchao/code/axon/src/worker.cpp)

Current limitations:

- IPv4 literal only
- no hostname resolution
- no IPv6 support

5. capability reporting

File:

- [src/config.cpp](/Users/wangyuchao/code/axon/src/config.cpp)

Current limitations:

- `supports_rma()` is effectively "context exists"
- `supports_memory_type()` only reports host memory as supported
- `supports_hw_tag_matching()` always returns `false`

6. atomic result path

Files:

- [src/endpoint.cpp](/Users/wangyuchao/code/axon/src/endpoint.cpp)
- [src/future.cpp](/Users/wangyuchao/code/axon/src/future.cpp)
- [include/zerokv/future.h](/Users/wangyuchao/code/axon/include/axon/future.h)

Current concern:

- atomic APIs are present, but the request/result plumbing still needs careful verification
- runtime validation in the current RDMA environment is not successful

## What Is Proven to Work Now

Based on the current QEMU VM validation:

- UCX 1.20 can be built and used successfully on the test VMs
- axon can be built against UCX 1.20
- TCP ping-pong works across the two VMs when pinned to the inter-VM device
- RDMA connection setup works over `rxe0`
- packed rkey exchange works
- RDMA put works
- RDMA get works
- put/get performance can be measured across the two VMs

## Practical Summary

Today, `axon` should be viewed as:

- a working UCX-based async transport core
- already capable of real two-node RDMA put/get
- still incomplete in advanced future composition, memory subsystem completeness, and some endpoint/control-plane semantics

The current implementation is strong enough for:

- transport experimentation
- UCX-based messaging and RMA validation
- building and testing RDMA workflows

It is not yet fully complete as a polished production-grade communication runtime.
