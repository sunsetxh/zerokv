# Data Model: AXON Transport Library

## Core Entities

### Config

Immutable configuration object built via fluent Builder API.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `transport` | `std::string` | `"ucx"` | Transport layer (ucx, tcp, rdma) |
| `num_workers` | `size_t` | `0` (auto) | Number of workers (0 = auto) |
| `memory_pool_size` | `size_t` | 64MB | Memory pool size |
| `max_inflight_requests` | `size_t` | `256` | Max pending requests |
| `connect_timeout` | `chrono::ms` | `10s` | Connection timeout |
| `registration_cache_enabled` | `bool` | `true` | Enable LRU cache |
| `registration_cache_max_entries` | `size_t` | `1024` | Cache size |

**Builder Methods**:
- `set_transport(std::string)`
- `set_num_workers(size_t)`
- `set_memory_pool_size(size_t)`
- `set_max_inflight_requests(size_t)`
- `set_connect_timeout(chrono::milliseconds)`
- `enable_registration_cache(bool)`
- `set_registration_cache_max_entries(size_t)`
- `set(std::string key, std::string value)` - UCX options
- `from_env()` - Populate from environment variables
- `build()` - Returns immutable Config

---

### Context

Shared handle to UCX context. All other objects depend on it.

| Method | Returns | Description |
|--------|---------|-------------|
| `create(const Config&)` | `Context::Ptr` | Factory method |
| `native_handle()` | `void*` | UCX ucp_context_h |
| `supports_rma()` | `bool` | RMA capability query |
| `supports_memory_type(MemoryType)` | `bool` | Memory support query |

---

### Worker

Progress engine bound to a single thread.

| Method | Returns | Description |
|--------|---------|-------------|
| `create(Context::Ptr, size_t index)` | `Worker::Ptr` | Factory |
| `progress()` | `bool` | Non-blocking progress |
| `wait(timeout)` | `bool` | Blocking wait |
| `run_until(pred)` | `void` | Event loop |
| `connect(addr)` | `Future<Endpoint::Ptr>` | Client connect |
| `listen(addr, callback)` | `Listener::Ptr` | Server listen |
| `event_fd()` | `int` | For asyncio integration |

---

### Endpoint

Connection handle for AXON communication.

| Method | Returns | Description |
|--------|---------|-------------|
| `tag_send(buffer, length, tag)` | `Future<void>` | Send with tag |
| `tag_send(MemoryRegion, tag)` | `Future<void>` | Send from registered memory |
| `tag_recv(buffer, length, tag, mask)` | `Future<pair<size_t, Tag>>` | Receive |
| `put(local, offset, remote_addr, rkey, len)` | `Future<void>` | RDMA put |
| `get(local, offset, remote_addr, rkey, len)` | `Future<void>` | RDMA get |
| `flush()` | `Future<void>` | Ensure completion |
| `close()` | `Future<void>` | Graceful close |
| `is_connected()` | `bool` | Connection state |

---

### Listener

Server-side connection acceptor.

| Method | Returns | Description |
|--------|---------|-------------|
| `listen(Worker::Ptr, addr, callback)` | `Listener::Ptr` | Factory |
| `close()` | `void` | Stop accepting |

**Accept Callback**: `void(Endpoint::Ptr)`

---

### MemoryRegion

Registered memory buffer.

| Method | Returns | Description |
|--------|---------|-------------|
| `register_mem(Context, addr, len, type)` | `MemoryRegion::Ptr` | Register existing |
| `allocate(Context, len, type)` | `MemoryRegion::Ptr` | Allocate + register |
| `address()` | `void*` | Buffer pointer |
| `length()` | `size_t` | Buffer size |
| `memory_type()` | `MemoryType` | Memory kind |
| `remote_key()` | `RemoteKey` | For RDMA |
| `as_span<T>()` | `span<T>` | Typed view |

---

### RemoteKey

Opaque handle to remote memory, serializable for transfer.

| Method | Returns | Description |
|--------|---------|-------------|
| `empty()` | `bool` | Valid key? |
| `size()` | `size_t` | Serialized size |
| `bytes()` | `const uint8_t*` | Serialized data |

---

### Future<T>

Async result wrapper.

| Method | Returns | Description |
|--------|---------|-------------|
| `ready()` | `bool` | Non-blocking poll |
| `get()` | `T` | Blocking get |
| `get(timeout)` | `optional<T>` | Timed get |
| `status()` | `Status` | Current status |
| `then(func)` | `Future<...>` | Chain continuation |
| `on_complete(callback)` | `void` | Callback on completion |

**Specializations**:
- `Future<void>` - Fire-and-forget
- `Future<size_t>` - Stream recv (bytes)
- `Future<pair<size_t, Tag>>` - Tag recv (bytes + tag)

---

### Request

Low-level UCX request handle.

| Method | Returns | Description |
|--------|---------|-------------|
| `is_complete()` | `bool` | Non-blocking test |
| `status()` | `Status` | Current status |
| `wait(timeout)` | `Status` | Blocking wait |
| `bytes_transferred()` | `size_t` | Transfer progress |

---

### Status / ErrorCode

Error handling.

| ErrorCode | Range | Description |
|-----------|-------|-------------|
| `kSuccess` | 0 | No error |
| `kInProgress` | 1 | Async pending |
| `kCanceled` | 2 | Operation canceled |
| `kTimeout` | 3 | Operation timed out |
| `kConnectionRefused` | 100 | Connect failed |
| `kConnectionReset` | 101 | Peer closed |
| `kTransportError` | 200 | UCX transport error |
| `kOutOfMemory` | 300 | Memory exhausted |
| `kRegistrationFailed` | 302 | Memory registration failed |
| `kInvalidArgument` | 900 | Bad parameters |
| `kInternalError` | 999 | Internal bug |

**Methods**:
- `ok()` - Success?
- `in_progress()` - Pending?
- `throw_if_error()` - Exception on error
- `std::error_code` integration

---

### Tag

64-bit tag for message matching.

| Constant | Value | Description |
|----------|-------|-------------|
| `kTagAny` | `UINT64_MAX` | Wildcard receive |
| `kTagMaskAll` | `UINT64_MAX` | Match exact tag |
| `kTagMaskUser` | `0xFFFFFFFF` | Match lower 32 bits |

**Helpers**:
- `make_tag(context, user)` - Create tag from components
- `tag_context(tag)` - Extract context bits
- `tag_user(tag)` - Extract user bits

---

### MemoryType

| Type | Description |
|------|-------------|
| `kHost` | System RAM |
| `kCuda` | NVIDIA GPU memory |
| `kROCm` | AMD GPU memory |
| `kAscend` | Huawei Ascend NPU |

---

## Relationships

```
Context (1)
  └─> Worker (N) - shared via weak_ptr
        ├─> Endpoint (N) - created by worker
        └─> Listener (N) - created by worker
              └─> Endpoint (N) - accepted

Context (1)
  └─> MemoryRegion (N) - registered memory

Endpoint (1)
  └─> MemoryRegion (N) - used for zero-copy
  └─> RemoteKey (N) - for RDMA operations

Future<T> (N)
  └─> Request (1) - underlying UCX handle
```
