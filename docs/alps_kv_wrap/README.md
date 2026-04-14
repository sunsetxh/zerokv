# ALPS KV Send/Recv Wrapper

This package provides a compatibility wrapper that keeps the `alps_kv_impl.cpp`
function shape but replaces the KV backend with ZeroKV `send/recv` transport.

## Artifacts

- `include/yr/alps_kv_api.h`
- `lib/libalps_kv_wrap.so`
- `bin/alps_kv_bench`

## Interface

The exported API matches the existing style:

```cpp
namespace YR {
int SetClient(const char* host, int port, int connect_timeout_ms);
void ShutdownClient();
int WriteBytes(const void* data, size_t size, int tag, int index, int src, int dst);
void ReadBytes(void* data, size_t size, int tag, int index, int src, int dst);
void ReadBytesBatch(std::vector<void*>& data,
                    const std::vector<size_t>& sizes,
                    const std::vector<int>& tags,
                    const std::vector<int>& indices,
                    const std::vector<int>& srcs,
                    const std::vector<int>& dsts);
}
```

## Connection Modes

`SetClient()` has two modes:

- client mode: `host` is a peer IP such as `127.0.0.1`
- listen mode: `host` is `0.0.0.0`, `listen`, or `server`

Examples:

```cpp
YR::SetClient("0.0.0.0", 16000, 5000);   // start listener
YR::SetClient("127.0.0.1", 16000, 5000); // connect to peer
```

## Semantics

- The backend is message transport, not persistent KV storage.
- `WriteBytes()` sends one framed message.
- `ReadBytes()` blocks until the matching `(tag, index, src, dst)` message arrives.
- `ReadBytesBatch()` performs sequential blocking reads for each requested message.
- Messages are consumed after a successful read.
- Repeated use of the same `(tag, index, src, dst)` is not handled as a stable multi-message queue. The benchmark and recommended usage keep each tuple unique.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target alps_kv_wrap alps_kv_bench
```

## Install

```bash
cmake --install build --prefix /tmp/alps-kv-package
```

Installed layout:

```text
/tmp/alps-kv-package/
  include/yr/alps_kv_api.h
  lib/libalps_kv_wrap.so
  bin/alps_kv_bench
  share/doc/alps_kv_wrap/README.md
```

## Transport Configuration

The transport is selected entirely via environment variables — no rebuild is
required to switch between TCP and RDMA.

| Variable | Purpose |
|---|---|
| `ZEROKV_TRANSPORT` | High-level preset: `tcp`, `rdma`, `rdma_ud`, `ucx` (default: `ucx` = auto) |
| `UCX_TLS` | UCX transport list, e.g. `rc,sm,self` for RoCE RC or `ud,sm,self` for UD |
| `UCX_NET_DEVICES` | Restrict to a specific network device, e.g. `rocep23s0f0:1` |
| `UCX_RNDV_THRESH` | Rendezvous threshold override, e.g. `65536` |

When none of these are set the library uses UCX auto-selection, which picks the
best available transport (RDMA if hardware is present, TCP otherwise).

### RoCE (25 GbE / InfiniBand over Ethernet)

Specify the device and use RC (Reliable Connected) transport for maximum
throughput:

```bash
export UCX_NET_DEVICES=rocep23s0f0:1   # your RoCE port
export UCX_TLS=rc,sm,self
```

Or equivalently via the ZeroKV preset:

```bash
export UCX_NET_DEVICES=rocep23s0f0:1
export ZEROKV_TRANSPORT=rdma
```

> **Important**: do **not** set `UCX_TLS=tcp` when `UCX_NET_DEVICES` points to
> a RoCE/IB device. TCP transport cannot bind to RDMA network devices and UCX
> will report `no able transports/devices`.

### Soft-RoCE (rxe, QEMU / simulation)

```bash
export UCX_PROTO_ENABLE=n
export UCX_NET_DEVICES=rxe0:1
export UCX_TLS=rc,sm,self
```

## Benchmark

### Basic (loopback / TCP)

Server:

```bash
./alps_kv_bench --mode server --port 16000 --sizes 256K,512K,1M,2M,4M,8M,16M,32M,64M --iters 100
```

Client:

```bash
./alps_kv_bench --mode client --host 127.0.0.1 --port 16000 --sizes 256K,512K,1M,2M,4M,8M,16M,32M,64M --iters 100
```

### RoCE two-node benchmark

```bash
# Node A (server / receiver)
UCX_NET_DEVICES=rocep23s0f0:1 UCX_TLS=rc,sm,self \
./alps_kv_bench --mode server --port 16000 \
    --sizes 256K,512K,1M,2M,4M,8M,16M,32M,64M --iters 100

# Node B (client / sender)
UCX_NET_DEVICES=rocep23s0f0:1 UCX_TLS=rc,sm,self \
./alps_kv_bench --mode client --host <server_ip> --port 16000 \
    --sizes 256K,512K,1M,2M,4M,8M,16M,32M,64M --iters 100
```

The benchmark uses unique `index` values for each iteration so it does not rely
on duplicate tuple handling.
You can still pass larger sizes such as `128M` manually when the runtime
environment has enough memory.
