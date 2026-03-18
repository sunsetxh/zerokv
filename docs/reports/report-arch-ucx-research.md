# AXON Transport Library — UCX Research Report

> Role: Technical Architect (Arch)
> Date: 2026-03-04
> Scope: C++20 library targeting Linux x86_64, UCX UCP layer

---

## Table of Contents

1. [UCX Best Practices for UCP Layer](#1-ucx-best-practices-for-ucp-layer)
   - 1.1 Worker Creation Patterns
   - 1.2 Endpoint Lifecycle
   - 1.3 Listener Patterns (Server-Side Accept)
   - 1.4 Tag-Matched Send/Recv API
   - 1.5 RMA (Put/Get/Flush) API Patterns
   - 1.6 Memory Registration
   - 1.7 Progress Engine Patterns
   - 1.8 Request Completion Callback Patterns
2. [CMake Build System for UCX-Based C++20 Libraries](#2-cmake-build-system)
   - 2.1 FindUCX.cmake Module Pattern
   - 2.2 Shared and Static Library Targets
   - 2.3 Google Test and Google Benchmark Integration
   - 2.4 spdlog Integration
3. [Error Handling Patterns for High-Performance C++ Libraries](#3-error-handling-patterns)
   - 3.1 Status/ErrorCode vs Exceptions
   - 3.2 std::error_code Integration
   - 3.3 Zero-Overhead Error Paths
4. [Future/Promise Pattern for UCX Async Operations](#4-futurepromise-pattern-for-ucx-async-operations)
   - 4.1 Wrapping UCX Requests into C++ Futures
   - 4.2 Avoiding Heap Allocation on the Fast Path
   - 4.3 Callback Chaining Patterns

---

## 1. UCX Best Practices for UCP Layer

### 1.1 Worker Creation Patterns

**Decision**: Use `UCS_THREAD_MODE_SINGLE`, one worker per thread, pinned to a fixed CPU core.

**Rationale**:

UCX exposes three thread-safety modes for `ucp_worker_h`:

| Mode | Semantics | Internal Locking | Performance |
|------|-----------|-----------------|-------------|
| `UCS_THREAD_MODE_SINGLE` | Only one thread ever touches the worker | None | Best |
| `UCS_THREAD_MODE_SERIALIZED` | Multiple threads, externally serialized | None | Medium |
| `UCS_THREAD_MODE_MULTI` | Multiple threads, UCX uses a giant internal lock | Giant lock | Worst |

`THREAD_MODE_MULTI` has poor scalability because UCX uses a single global lock over the entire worker state machine. The lock is acquired on every `ucp_worker_progress()` call, every send/recv posting, and every event. At 64+ concurrent operations this lock becomes the dominant bottleneck.

The correct pattern for multi-threaded workloads is to create N workers (one per thread) all in `SINGLE` mode, then assign endpoints to workers. Cross-thread work submission uses a lock-free MPSC queue and a Linux `eventfd` for wakeup.

**Initialization Sequence**:

```c
// 1. Initialize UCP context (once, process-wide)
ucp_params_t ctx_params = {};
ctx_params.field_mask  = UCP_PARAM_FIELD_FEATURES
                       | UCP_PARAM_FIELD_REQUEST_SIZE
                       | UCP_PARAM_FIELD_REQUEST_INIT;
ctx_params.features    = UCP_FEATURE_TAG
                       | UCP_FEATURE_RMA
                       | UCP_FEATURE_AMO64
                       | UCP_FEATURE_WAKEUP;   // required for ucp_worker_wait
ctx_params.request_size = sizeof(RequestContext);
ctx_params.request_init = request_init_cb;

ucp_config_t* config;
ucp_config_read(NULL, NULL, &config);
ucp_init(&ctx_params, config, &ucp_context);
ucp_config_release(config);

// 2. Create a worker per thread
ucp_worker_params_t wparams = {};
wparams.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
wparams.thread_mode = UCS_THREAD_MODE_SINGLE;

ucp_worker_h worker;
ucp_worker_create(ucp_context, &wparams, &worker);

// 3. Verify the actual mode granted
ucp_worker_attr_t attr = {};
attr.field_mask = UCP_WORKER_ATTR_FIELD_THREAD_MODE;
ucp_worker_query(worker, &attr);
assert(attr.thread_mode == UCS_THREAD_MODE_SINGLE);

// 4. Obtain the event fd for epoll (if WAKEUP feature was set)
int efd;
ucs_status_t s = ucp_worker_get_efd(worker, &efd);
// efd == -1 if transport does not support WAKEUP
```

**Note on UCP_FEATURE_WAKEUP**: This feature flag is required to use `ucp_worker_wait()` and `ucp_worker_get_efd()`. It must be declared at `ucp_init` time; it cannot be added retroactively.

**NUMA / CPU Affinity**:

For NUMA-sensitive workloads, each worker should be bound to a specific NUMA node. The worker's thread must be bound to the same NUMA node as the NIC that handles its connections. Mismatched NUMA bindings can add 1-3 us of latency on large-memory servers.

```c
// Platform-specific CPU affinity (Linux)
cpu_set_t cpuset;
CPU_ZERO(&cpuset);
CPU_SET(target_cpu, &cpuset);
pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
```

**Alternatives Considered**:

| Alternative | Why Not Chosen |
|-------------|----------------|
| `THREAD_MODE_MULTI` | Giant internal lock kills scalability beyond 2 threads |
| Single shared worker with external mutex | Serializes all progress; same problem as MULTI mode |
| Thread pool with work stealing | Complex and unnecessary; endpoint-per-worker is simpler and as fast |

---

### 1.2 Endpoint Lifecycle

**Decision**: Lazy connect (non-blocking `ucp_ep_create`), explicit graceful close via `ucp_ep_close_nbx`, always set an error handler.

**Rationale**:

The UCX endpoint lifecycle has four stages:

```
ucp_ep_create()          ->  CREATED (not yet connected)
    |
    v  (first data operation or explicit flush triggers handshake)
CONNECTING
    |
    v  (UCX transport completes the underlying connection)
CONNECTED  <---> operations flow (tag_send, put, get, ...)
    |
    v  ucp_ep_close_nbx(UCP_EP_CLOSE_FLAG_FLUSH)
CLOSING
    |
    v  (remote side also closes)
CLOSED
```

**Endpoint Creation**:

UCX endpoint creation is non-blocking. The actual transport-level connection is established lazily on first use, or can be forced by calling `ucp_ep_flush_nb`. This means `ucp_ep_create` is safe to call in a hot path.

```c
// Connecting by socket address (preferred: no out-of-band address exchange needed)
ucp_ep_params_t ep_params = {};
ep_params.field_mask = UCP_EP_PARAM_FIELD_FLAGS
                     | UCP_EP_PARAM_FIELD_SOCK_ADDR
                     | UCP_EP_PARAM_FIELD_ERR_HANDLER
                     | UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;

ep_params.flags               = UCP_EP_PARAMS_FLAGS_CLIENT_SERVER;
ep_params.err_handler.cb      = endpoint_error_callback;
ep_params.err_handler.arg     = user_context;
ep_params.err_mode            = UCP_ERR_HANDLING_MODE_PEER;

struct sockaddr_storage addr = /* ... fill from host:port ... */;
ep_params.sockaddr.addr    = (const struct sockaddr*)&addr;
ep_params.sockaddr.addrlen = sizeof(addr);

ucp_ep_h ep;
ucp_ep_create(worker, &ep_params, &ep);
```

**Error Handling Mode**: `UCP_ERR_HANDLING_MODE_PEER` is required for peer-failure detection. Without it, UCX may silently discard operations on a dead endpoint. This flag has a small overhead (enables UCX's transport-level keepalive path), but is essential for correctness in production.

**Graceful Close**:

```c
// Must use ucp_ep_close_nbx (replaces deprecated ucp_ep_destroy)
ucp_request_param_t close_params = {};
close_params.op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS;
close_params.flags        = UCP_EP_CLOSE_FLAG_FLUSH;  // flush pending sends first

void* req = ucp_ep_close_nbx(ep, &close_params);
if (req == NULL) {
    // immediate completion
} else if (UCS_PTR_IS_ERR(req)) {
    // error during close; log and continue
} else {
    // poll until complete
    while (ucp_request_check_status(req) == UCS_INPROGRESS) {
        ucp_worker_progress(worker);
    }
    ucp_request_free(req);
}
```

**Force Close**: If a peer is known to be dead and flush is not possible, use `UCP_EP_CLOSE_FLAG_FORCE` instead of `UCP_EP_CLOSE_FLAG_FLUSH`. Force close discards pending operations.

**Error Callback**:

The error callback fires from within `ucp_worker_progress()` on the worker thread. It must not call any UCX operations on the faulted endpoint. Its only safe actions are: record the error, signal application state, and schedule cleanup to happen after progress returns.

```c
static void endpoint_error_callback(void* arg, ucp_ep_h ep, ucs_status_t status) {
    // MUST be called from the worker thread (inside ucp_worker_progress)
    // DO NOT call ucp_ep_close/destroy from here
    MyEndpointContext* ctx = (MyEndpointContext*)arg;
    ctx->error_status = status;
    ctx->connected    = false;
    // Signal to the progress loop to clean up after progress() returns
    ctx->needs_cleanup.store(true, std::memory_order_release);
}
```

**Alternatives Considered**:

| Alternative | Why Not Chosen |
|-------------|----------------|
| `ucp_ep_destroy()` (deprecated) | Does not flush; lost messages possible |
| Connect-by-worker-address | Requires out-of-band address exchange mechanism |
| No error handler | Silently lost messages on peer failure |

---

### 1.3 Listener Patterns (Server-Side Accept)

**Decision**: Use `ucp_listener_create()` with a connection handler callback; accept connections with `ucp_ep_create` using the `conn_request` parameter.

**Rationale**:

UCX provides a built-in listener for socket-based connection establishment. This avoids the need for an external bootstrap channel for the common client-server pattern.

```c
// Server side: create listener
static void connection_handler(ucp_conn_request_h conn_req, void* arg) {
    // This callback is called from inside ucp_worker_progress()
    // on the listener's worker thread
    MyServer* server = (MyServer*)arg;

    // Inspect the connecting client's address
    ucp_conn_request_attr_t req_attr = {};
    req_attr.field_mask = UCP_CONN_REQ_ATTR_FIELD_CLIENT_ADDR;
    ucp_conn_request_query(conn_req, &req_attr);

    // Accept: create an endpoint for this incoming connection
    ucp_ep_params_t ep_params = {};
    ep_params.field_mask   = UCP_EP_PARAM_FIELD_CONN_REQUEST
                           | UCP_EP_PARAM_FIELD_ERR_HANDLER
                           | UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
    ep_params.conn_request = conn_req;
    ep_params.err_handler.cb  = endpoint_error_callback;
    ep_params.err_handler.arg = /* per-endpoint context */;
    ep_params.err_mode        = UCP_ERR_HANDLING_MODE_PEER;

    ucp_ep_h ep;
    ucs_status_t status = ucp_ep_create(server->worker, &ep_params, &ep);
    if (status != UCS_OK) { /* reject */ return; }

    server->on_accept(ep);
}

// Create listener
ucp_listener_params_t lparams = {};
lparams.field_mask          = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR
                            | UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;
lparams.conn_handler.cb     = connection_handler;
lparams.conn_handler.arg    = server;

struct sockaddr_in addr = {};
addr.sin_family      = AF_INET;
addr.sin_port        = htons(port);        // port 0 = OS-assigned
addr.sin_addr.s_addr = INADDR_ANY;
lparams.sockaddr.addr    = (const struct sockaddr*)&addr;
lparams.sockaddr.addrlen = sizeof(addr);

ucp_listener_h listener;
ucp_listener_create(worker, &lparams, &listener);

// Query the actual bound port (if port=0 was used)
ucp_listener_attr_t lattr = {};
lattr.field_mask = UCP_LISTENER_ATTR_FIELD_SOCKADDR;
ucp_listener_query(listener, &lattr);
uint16_t actual_port = ntohs(((struct sockaddr_in*)&lattr.sockaddr)->sin_port);
```

**Key Points**:

- The `connection_handler` callback executes on the listener's worker thread, inside `ucp_worker_progress`. It must complete quickly.
- The `conn_request` passed to the callback is only valid for the duration of the callback, or until `ucp_ep_create` consumes it. Do not store it.
- To reject a connection, call `ucp_listener_reject(listener, conn_req)` instead of accepting.
- One listener can serve connections to multiple worker threads; after `ucp_ep_create` accepts the connection, the new endpoint is owned by the worker passed to `ucp_ep_create`. In a multi-worker design, the listener worker can round-robin new connections to application workers.

**Server-Side Worker Separation**:

A common pattern is to have a dedicated "listener worker" that only runs the listener and connection handler, and dispatches accepted endpoints to application workers:

```
Listener Worker (thread 0):
  ucp_listener_h listener
  connection_handler() -> ucp_ep_create on app worker[N % num_workers]

App Worker 0 (thread 1): owns Endpoint 0, 3, 6, ...
App Worker 1 (thread 2): owns Endpoint 1, 4, 7, ...
App Worker 2 (thread 3): owns Endpoint 2, 5, 8, ...
```

Cross-worker endpoint assignment is achieved via the MPSC queue described in section 1.1.

**Alternatives Considered**:

| Alternative | Why Not Chosen |
|-------------|----------------|
| External rendezvous server (e.g., Redis) for address exchange | More infrastructure; UCX listener is self-contained |
| MPI-style address exchange (all-gather of worker addresses) | Only valid when all ranks are known at start; not general |
| `UCP_LISTENER_PARAM_FIELD_ACCEPT_HANDLER` (deprecated) | Replaced by connection handler + `ucp_ep_create` in UCX >= 1.7 |

---

### 1.4 Tag-Matched Send/Recv API

**Decision**: Use `ucp_tag_send_nbx` and `ucp_tag_recv_nbx` (the `_nbx` unified API introduced in UCX 1.9), with `UCP_OP_ATTR_FIELD_CALLBACK` for async completion.

**Rationale**:

UCX has evolved through three generations of async send/recv APIs:

| Generation | API | Status | Notes |
|-----------|-----|--------|-------|
| 1st | `ucp_tag_send_nb`, `ucp_tag_recv_nb` | Deprecated | Callback-only |
| 2nd | `ucp_tag_send_nbr`, `ucp_tag_recv_nbr` | Superseded | Inline request |
| 3rd | `ucp_tag_send_nbx`, `ucp_tag_recv_nbx` | Current (UCX 1.9+) | Unified params |

The `_nbx` API uses a single `ucp_request_param_t` struct that unifies all options (callbacks, user data, memory type, flags). This is the only API that will receive new features in future UCX releases.

**Tag Send**:

```c
// Callback-based send (async, non-blocking)
static void send_callback(void* request, ucs_status_t status, void* user_data) {
    // Called from within ucp_worker_progress() on the worker thread
    // request == the UCX request pointer (same as ucp_tag_send_nbx return value)
    // user_data == whatever was set in request_param.user_data
    MyRequest* my_req = (MyRequest*)user_data;
    my_req->status    = status;
    my_req->completed = true;
    ucp_request_free(request);   // MUST free here if not using custom request init
}

ucp_request_param_t send_params = {};
send_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK
                         | UCP_OP_ATTR_FIELD_USER_DATA;
send_params.cb.send      = send_callback;
send_params.user_data    = my_request_context;

void* req = ucp_tag_send_nbx(ep, buffer, length, tag, &send_params);

if (req == NULL) {
    // Immediate completion (inline / eager short path)
    // send_callback will NOT be called
    my_request_context->status    = UCS_OK;
    my_request_context->completed = true;
} else if (UCS_PTR_IS_ERR(req)) {
    // Immediate error
    my_request_context->status    = UCS_PTR_STATUS(req);
    my_request_context->completed = true;
} else {
    // Pending: send_callback will be called from ucp_worker_progress
}
```

**Tag Receive**:

```c
static void recv_callback(void* request, ucs_status_t status,
                          const ucp_tag_recv_info_t* tag_info, void* user_data) {
    MyRequest* my_req      = (MyRequest*)user_data;
    my_req->status         = status;
    my_req->bytes_received = tag_info->length;
    my_req->matched_tag    = tag_info->sender_tag;
    my_req->completed      = true;
    ucp_request_free(request);
}

ucp_request_param_t recv_params = {};
recv_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK
                         | UCP_OP_ATTR_FIELD_USER_DATA;
recv_params.cb.recv      = recv_callback;
recv_params.user_data    = my_request_context;

// Tag and mask: receive any message whose (tag & tag_mask) == (expected_tag & tag_mask)
void* req = ucp_tag_recv_nbx(worker, buffer, length,
                              expected_tag, tag_mask, &recv_params);
// Same NULL / ERR / pending handling as send
```

**Tag Mask Usage**:

UCX tag matching uses a 64-bit tag with a 64-bit mask. Our project convention:
- Bits [63:32]: communicator / context ID (match exactly)
- Bits [31:0]:  user message tag (flexible)

```c
// Match only messages from context 42, any user tag
ucp_tag_t tag      = ((uint64_t)42 << 32) | 0;
ucp_tag_t tag_mask = 0xFFFFFFFF00000000ULL;  // match upper 32 bits only
```

**GPU Memory (CUDA)**:

To send from or receive into GPU memory buffers, add the memory type hint:

```c
send_params.op_attr_mask |= UCP_OP_ATTR_FIELD_MEMTYPE;
send_params.memtype       = UCS_MEMORY_TYPE_CUDA;
```

This hint enables UCX to select the optimal transport path (e.g., GPUDirect RDMA via `cuda_ipc` or `rc_mlx5`) without performing a slow `cudaPointerGetAttributes` call on every message.

**Eager vs Rendezvous Protocol Selection**:

UCX internally selects the protocol:

| Size | Protocol | Behavior |
|------|----------|----------|
| < `UCX_BCOPY_THRESH` (default 8 KB) | Eager short / bcopy | Data copied into network buffer inline |
| `UCX_BCOPY_THRESH` to `UCX_ZCOPY_THRESH` | Eager zcopy | Zero-copy if memory is registered |
| > `UCX_RNDV_THRESH` (default 256 KB) | Rendezvous | 3-way handshake + RDMA read |

For rendezvous to be truly zero-copy, the send buffer must be registered (`ucp_mem_map` has been called) before the send. For pre-registered `MemoryRegion` objects this is already satisfied.

**Alternatives Considered**:

| Alternative | Why Not Chosen |
|-------------|----------------|
| `ucp_tag_send_nb` (1st gen) | Deprecated; does not support user_data in callback; will be removed |
| UCX Active Messages (`ucp_am_send_nbx`) | Better for request-response patterns but no tag matching |
| UCX Stream API (`ucp_stream_send_nbx`) | Ordered byte stream with no tags; different semantics |

---

### 1.5 RMA (Put/Get/Flush) API Patterns

**Decision**: Use `ucp_put_nbx` / `ucp_get_nbx` for one-sided operations, `ucp_ep_flush_nbx` for ordering guarantees, and always exchange remote keys via `ucp_rkey_pack` / `ucp_ep_rkey_unpack`.

**Rationale**:

UCX RMA operations are one-sided: the initiator drives the transfer with no active participation from the remote side. The remote side only needs to have registered the memory and exchanged the remote key (rkey).

**Remote Key Exchange** (done once per region):

```c
// On the memory-owner side: pack the rkey
ucp_mem_h mem_handle;   // obtained from ucp_mem_map (see section 1.6)

void*  rkey_buffer;
size_t rkey_size;
ucp_rkey_pack(ucp_context, mem_handle, &rkey_buffer, &rkey_size);

// Transmit rkey_buffer (size rkey_size bytes) to the remote peer
// via any channel (tag_send, OOB network, etc.)
send_rkey_to_peer(rkey_buffer, rkey_size);
ucp_rkey_buffer_release(rkey_buffer);   // release the packed buffer after sending

// On the RMA initiator side: unpack the received bytes
ucp_rkey_h rkey;
ucp_ep_rkey_unpack(ep, received_rkey_bytes, &rkey);
// rkey is now bound to this endpoint; use for put/get operations
```

**Put (write to remote)**:

```c
static void put_callback(void* request, ucs_status_t status, void* user_data) {
    MyRequest* r = (MyRequest*)user_data;
    r->status    = status;
    r->completed = true;
    ucp_request_free(request);
}

ucp_request_param_t put_params = {};
put_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK
                        | UCP_OP_ATTR_FIELD_USER_DATA;
put_params.cb.send      = put_callback;   // put uses the send callback signature
put_params.user_data    = req_ctx;

// local_addr: local virtual address (must be registered for zero-copy)
// remote_addr: remote virtual address obtained from ucp_mem_attr_t.address
// rkey: unpacked remote key (from ucp_ep_rkey_unpack)
void* req = ucp_put_nbx(ep, local_addr, length, remote_addr, rkey, &put_params);
```

**Get (read from remote)**:

```c
ucp_request_param_t get_params = {};
get_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK
                        | UCP_OP_ATTR_FIELD_USER_DATA;
get_params.cb.send      = get_callback;
get_params.user_data    = req_ctx;

void* req = ucp_get_nbx(ep, local_addr, length, remote_addr, rkey, &get_params);
```

**Flush (ordering guarantee)**:

RMA operations on a given endpoint are not guaranteed to be visible on the remote side until a flush completes. The flush forces all outstanding puts/gets on the endpoint to reach the remote memory before the flush callback fires.

```c
// Per-endpoint flush (most common)
static void flush_callback(void* request, ucs_status_t status, void* user_data) {
    MyRequest* r = (MyRequest*)user_data;
    r->status    = status;
    r->completed = true;
    ucp_request_free(request);
}

ucp_request_param_t flush_params = {};
flush_params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK
                          | UCP_OP_ATTR_FIELD_USER_DATA;
flush_params.cb.send      = flush_callback;
flush_params.user_data    = req_ctx;

void* req = ucp_ep_flush_nbx(ep, &flush_params);

// Worker-level flush (all endpoints on this worker)
void* req = ucp_worker_flush_nbx(worker, &flush_params);
```

**Memory Visibility Semantics**:

- `ucp_put_nbx` completion callback fires when the data has left the local NIC buffer. It does NOT guarantee visibility on the remote side.
- `ucp_ep_flush_nbx` completion guarantees that all prior puts on this endpoint are visible in remote memory.
- For InfiniBand RC transport: put + flush achieves true remote completion with hardware ordering.
- For shared memory (CMA path): UCX issues memory barriers internally.

**Atomics**:

UCX provides 64-bit atomic RMA operations:

```c
// Fetch-and-add: remote_addr += value; returns old value
void* req = ucp_atomic_fetch_nbx(ep, UCP_ATOMIC_FETCH_OP_FADD,
                                  value, &result_64,
                                  remote_addr, sizeof(uint64_t), rkey,
                                  &params);

// Compare-and-swap: if (*remote_addr == compare) { *remote_addr = swap }
void* req = ucp_atomic_op_nbx(ep, UCP_ATOMIC_OP_CSWAP,
                               &compare_and_swap_val,
                               remote_addr, sizeof(uint64_t), rkey,
                               &params);
```

Atomics require `UCP_FEATURE_AMO64` at context initialization.

**Alternatives Considered**:

| Alternative | Why Not Chosen |
|-------------|----------------|
| `ucp_put_nb` (deprecated) | Same as tag; deprecated API |
| UCT-level verbs atomics | Bypasses UCP transport selection; requires manual multi-rail handling |
| Software emulated atomics over tag messages | Higher latency; breaks the one-sided model |

---

### 1.6 Memory Registration

**Decision**: Use `ucp_mem_map` with `UCP_MEM_MAP_ALLOCATE` for library-managed buffers, and `ucp_mem_map` without `ALLOCATE` for user-provided buffers; cache registrations using an LRU interval tree.

**Rationale**:

Memory registration (pinning) is the process of locking pages in physical memory and registering them with the NIC's RDMA subsystem. It is an expensive operation (10-100 us per call on typical hardware) that must be amortized.

**Registration of Existing Buffer**:

```c
ucp_mem_map_params_t params = {};
params.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS
                  | UCP_MEM_MAP_PARAM_FIELD_LENGTH
                  | UCP_MEM_MAP_PARAM_FIELD_MEMORY_TYPE;  // for GPU memory
params.address    = user_buffer;
params.length     = buffer_length;
params.memory_type = UCS_MEMORY_TYPE_HOST;  // or CUDA, ROCM, etc.

ucp_mem_h mem_handle;
ucs_status_t status = ucp_mem_map(ucp_context, &params, &mem_handle);

// Query actual registered region (may be larger than requested due to page alignment)
ucp_mem_attr_t attr = {};
attr.field_mask = UCP_MEM_ATTR_FIELD_ADDRESS
               | UCP_MEM_ATTR_FIELD_LENGTH;
ucp_mem_query(mem_handle, &attr);
// attr.address and attr.length are the actual registered range
```

**Library-Allocated + Registered Buffer**:

```c
ucp_mem_map_params_t params = {};
params.field_mask = UCP_MEM_MAP_PARAM_FIELD_LENGTH
                  | UCP_MEM_MAP_PARAM_FIELD_FLAGS;
params.length     = allocation_size;
params.flags      = UCP_MEM_MAP_ALLOCATE;  // UCX allocates and registers

ucp_mem_h mem_handle;
ucp_mem_map(ucp_context, &params, &mem_handle);

// Retrieve the allocated pointer
ucp_mem_attr_t attr = {};
attr.field_mask = UCP_MEM_ATTR_FIELD_ADDRESS;
ucp_mem_query(mem_handle, &attr);
void* registered_ptr = attr.address;
```

**Deregistration**:

```c
ucp_mem_unmap(ucp_context, mem_handle);
// After this call, the memory is unpinned and NIC registration is released
// All outstanding operations using this mem_handle must have completed first
```

**Remote Key Pack/Unpack** (for RMA):

```c
// Pack on the memory owner
void* rkey_buf;
size_t rkey_size;
ucp_rkey_pack(ucp_context, mem_handle, &rkey_buf, &rkey_size);
// ... transmit rkey_buf[0..rkey_size) to the RMA initiator ...
ucp_rkey_buffer_release(rkey_buf);

// Unpack on the initiator, bound to an endpoint
ucp_rkey_h rkey;
ucp_ep_rkey_unpack(ep, received_bytes, &rkey);
// ... use rkey for put/get ...
ucp_rkey_destroy(rkey);   // release when done
```

**Registration Cache Design**:

For the library's internal registration cache, the key data structures are:

```cpp
// Key: (virtual address range, memory type)
// Data structure: interval tree for O(log n) range overlap queries
struct RegCacheEntry {
    uintptr_t        addr;
    size_t           length;
    MemoryType       mem_type;
    ucp_mem_h        mem_handle;
    std::atomic<int> ref_count{0};  // protect against eviction while in-flight
    uint64_t         last_access;   // for LRU ordering
};
```

Lookup algorithm:
1. Query interval tree for entries overlapping `[addr, addr+len)`.
2. If exact match found (same addr, len, mem_type): cache hit.
3. If partial overlap (sub-range already registered): merge by deregistering old range, registering the union.
4. No overlap: cache miss, call `ucp_mem_map`, insert into tree.
5. On eviction: check `ref_count == 0`, then call `ucp_mem_unmap`.

**CUDA Memory Considerations**:

For CUDA memory, UCX can optionally register on the base allocation address rather than the exact pointer when `UCX_MEMTYPE_REG_WHOLE_ALLOC_TYPES=cuda` is set. This dramatically improves cache hit rates for subbuffer pointers into a large `cudaMalloc` allocation.

**Alternatives Considered**:

| Alternative | Why Not Chosen |
|-------------|----------------|
| No cache (register per-operation) | 10-100 us overhead per operation; unacceptable for small messages |
| Simple hash map keyed on exact pointer | Misses partial overlaps; registers same memory multiple times |
| Pre-registered memory pool only | Limits user flexibility; cannot handle arbitrary user buffers |

---

### 1.7 Progress Engine Patterns

**Decision**: Tight-loop `ucp_worker_progress` for throughput workloads; hybrid poll-then-wait (with `ucp_worker_wait` and `ucp_worker_get_efd`) for latency-sensitive and event-driven workloads.

**Rationale**:

UCX does not have background threads. All protocol progress (sending/receiving packet headers, processing completions, invoking callbacks) happens inside `ucp_worker_progress`. This must be called regularly by the application.

**`ucp_worker_progress` (polling)**:

```c
// Returns non-zero if any progress was made (completions processed, packets sent)
// Returns 0 if there was nothing to do
unsigned progressed = ucp_worker_progress(worker);
```

This is a non-blocking call. It processes all pending events in one pass. For maximum throughput, call it in a tight loop.

**`ucp_worker_wait` (blocking)**:

```c
// Block until the worker has events to process, or an external wakeup occurs
// Uses an OS-level wait (epoll_wait internally)
// timeout is not directly controllable via ucp_worker_wait; use the efd for that
ucs_status_t status = ucp_worker_wait(worker);
```

`ucp_worker_wait` is a blocking call, suitable only for the "nothing to do" state. It requires `UCP_FEATURE_WAKEUP` at initialization.

**Hybrid Pattern (recommended for most workloads)**:

```c
// Progress loop: spin briefly, then sleep if idle
void worker_run_loop(ucp_worker_h worker, std::atomic<bool>& stop_flag) {
    constexpr int SPIN_COUNT = 1000;

    while (!stop_flag.load(std::memory_order_relaxed)) {
        // Drain pending cross-thread submissions first
        drain_submission_queue();

        // Spin for a few iterations (catches bursts efficiently)
        int spin = SPIN_COUNT;
        while (spin-- > 0 && ucp_worker_progress(worker) != 0) {
            // keep progressing while there's work
        }

        // If still no progress, sleep via epoll
        if (ucp_worker_progress(worker) == 0) {
            ucp_worker_wait(worker);
        }
    }
}
```

**epoll Integration** (for asyncio / event loops):

```c
int efd;
ucp_worker_get_efd(worker, &efd);
// Register efd with epoll/select/io_uring:
//   When efd becomes readable, call ucp_worker_progress until it returns 0
//   Then call ucp_worker_arm() to re-enable the notification
```

**Critical**: After processing all pending events, call `ucp_worker_arm` before sleeping. Failing to do this results in missed wakeups:

```c
void event_driven_progress(ucp_worker_h worker) {
    while (ucp_worker_progress(worker) != 0) {
        // drain all pending progress
    }
    // Arm before sleeping; this establishes the "edge-triggered" behavior
    ucs_status_t arm_status = ucp_worker_arm(worker);
    if (arm_status == UCS_ERR_BUSY) {
        // New events arrived between last progress call and arm;
        // do not sleep, loop back immediately
        return;
    }
    // Now safe to epoll_wait on efd
}
```

**Performance Profile**:

| Approach | CPU Usage | Latency | Best For |
|----------|-----------|---------|---------|
| Tight spin loop | 100% 1 core | Lowest (~0.5-1 us added) | Dedicated progress thread, throughput |
| Hybrid (spin + wait) | ~5-20% | Low (~1-2 us added) | Balanced; most production use cases |
| Pure event-driven (epoll) | ~0% when idle | Higher (~5-10 us added) | CPU-constrained, many idle workers |

**`ucp_worker_signal` (external wakeup)**:

To wake a worker blocked in `ucp_worker_wait` from another thread:

```c
ucp_worker_signal(worker);  // safe to call from any thread
```

In the project's MPSC-queue cross-thread design, we instead write to the `eventfd` directly:

```c
uint64_t val = 1;
write(event_fd, &val, sizeof(val));  // wakes epoll_wait on the worker thread
```

**Alternatives Considered**:

| Alternative | Why Not Chosen |
|-------------|----------------|
| UCX background progress thread (`UCS_ASYNC_MODE_THREAD`) | UCX internal; not reliable across transports; bypasses our control |
| io_uring for event loop | Adds complexity; UCX's efd is already a single fd to poll |
| Busy-polling only | Burns a full CPU core when idle; not acceptable for server deployments |

---

### 1.8 Request Completion Callback Patterns

**Decision**: Use UCX request initialization hooks (`ucp_params_t.request_init`) to embed per-request state inline with the UCX request object; pass `user_data` through `ucp_request_param_t` to avoid an allocation.

**Rationale**:

Each in-flight UCX operation has an associated "request" — an opaque memory block allocated by UCX. The size and initialization of this block can be customized at `ucp_init` time.

**Custom Request Layout**:

```c
// Define a layout for the embedded state
struct RequestContext {
    // Embedded at the *start* of the UCX request block
    // (UCX allocates sizeof(ucp_request_t) + request_size bytes)
    std::atomic<bool>  completed{false};
    ucs_status_t       status{UCS_INPROGRESS};
    size_t             bytes_transferred{0};
    ucp_tag_t          matched_tag{0};
    // C++ Future/Promise linkage (pointer, not inline, to avoid growing request block)
    void*              promise_ptr{nullptr};
};

static void request_init(void* request) {
    // UCX calls this when allocating a new request from its pool
    RequestContext* ctx = static_cast<RequestContext*>(request);
    new (ctx) RequestContext{};  // placement new for C++ construction
}

static void request_cleanup(void* request) {
    // Called before returning a request to UCX's pool
    RequestContext* ctx = static_cast<RequestContext*>(request);
    ctx->~RequestContext();
}

// Register at context init time
ucp_params_t params = {};
params.field_mask     |= UCP_PARAM_FIELD_REQUEST_SIZE
                       | UCP_PARAM_FIELD_REQUEST_INIT
                       | UCP_PARAM_FIELD_REQUEST_CLEANUP;
params.request_size    = sizeof(RequestContext);
params.request_init    = request_init;
params.request_cleanup = request_cleanup;
```

**Completion Callback Pattern**:

```c
// The user_data pointer is passed directly through ucp_request_param_t
// and received in the callback; no allocation required on the fast path.
static void tag_send_callback(void* ucx_request, ucs_status_t status,
                               void* user_data) {
    // user_data is whatever was set in request_param.user_data
    // For our Future<void> model, user_data IS the promise pointer
    auto* promise = static_cast<axon::internal::SendPromise*>(user_data);
    promise->set_result(status == UCS_OK
                        ? axon::Status::OK()
                        : axon::Status{ucx_to_axon_error(status)});
    // promise->set_result() atomically marks the future as ready
    // The Future's .get() / .ready() poll will see this

    // Free the UCX request back to UCX's pool
    ucp_request_free(ucx_request);
}
```

**Immediate Completion Fast Path**:

When `ucp_tag_send_nbx` returns `NULL` (immediate completion), the callback is NOT called. The caller must handle this case explicitly, which is the critical zero-allocation fast path:

```c
void* req = ucp_tag_send_nbx(ep, buf, len, tag, &params);
if (req == NULL) {
    // Fast path: already done, no callback, no heap allocation for this operation
    promise->set_result(Status::OK());
} else if (UCS_PTR_IS_ERR(req)) {
    promise->set_result(Status{ucx_to_axon_error(UCS_PTR_STATUS(req))});
} else {
    // Slow path: will complete via callback
    // The UCX request is allocated from UCX's internal pool (not malloc)
    // Our RequestContext is embedded in that block (no extra heap allocation)
}
```

**Callback Threading Rules**:

All completion callbacks fire from within `ucp_worker_progress()` on the worker thread. Key constraints:
1. Callbacks must not call `ucp_worker_progress` recursively.
2. Callbacks must not block (no mutex acquisition, no IO).
3. Callbacks must not create or destroy UCX endpoints.
4. Callbacks may call other UCX operations (e.g., post the next recv after a recv completes), but this is advanced usage.

**Alternatives Considered**:

| Alternative | Why Not Chosen |
|-------------|----------------|
| Allocate a heap `std::promise` per operation | Heap allocation on every send/recv; ~50-200 ns malloc overhead |
| Store state in a side table (map from request ptr to state) | Hash map lookup on every completion; higher overhead; not cache-friendly |
| `std::future` / `std::promise` standard types | Not allocator-aware; always heap-allocate; no progress polling |

---

## 2. CMake Build System

### 2.1 FindUCX.cmake Module Pattern

**Decision**: Prefer `pkg-config` (`PkgConfig::UCX`) for standard installations; fall back to a custom `FindUCX.cmake` that searches common UCX installation paths.

**Rationale**:

UCX ships with both a `.pc` (pkg-config) file and a CMake config package starting from version 1.11. However, many HPC clusters install UCX in non-standard paths (e.g., `/opt/ucx`, `/usr/local/ucx`, spack-managed paths) where CMake's automatic discovery fails.

**Option A: pkg-config (current project approach)**:

```cmake
# CMakeLists.txt (current approach in this project)
find_package(PkgConfig REQUIRED)
pkg_check_modules(UCX REQUIRED IMPORTED_TARGET ucx)
# Provides: PkgConfig::UCX target with include dirs, link libs, and compiler flags
target_link_libraries(axon PUBLIC PkgConfig::UCX pthread)
```

This works reliably when `PKG_CONFIG_PATH` is set to the UCX installation's `lib/pkgconfig` directory.

**Option B: Custom FindUCX.cmake (for sites without pkg-config)**:

```cmake
# cmake/FindUCX.cmake
cmake_minimum_required(VERSION 3.20)

# Search hints: environment variable UCX_ROOT takes precedence
set(_UCX_HINTS
    ${UCX_ROOT}
    $ENV{UCX_ROOT}
    /usr/local/ucx
    /opt/ucx
    /usr
)

find_path(UCX_INCLUDE_DIR
    NAMES ucp/api/ucp.h
    HINTS ${_UCX_HINTS}
    PATH_SUFFIXES include
    DOC "UCX include directory"
)

find_library(UCX_UCP_LIBRARY
    NAMES ucp
    HINTS ${_UCX_HINTS}
    PATH_SUFFIXES lib lib64
    DOC "UCX UCP library"
)

find_library(UCX_UCT_LIBRARY
    NAMES uct
    HINTS ${_UCX_HINTS}
    PATH_SUFFIXES lib lib64
)

find_library(UCX_UCS_LIBRARY
    NAMES ucs
    HINTS ${_UCX_HINTS}
    PATH_SUFFIXES lib lib64
)

# Extract version from ucs/version.h
if(UCX_INCLUDE_DIR AND EXISTS "${UCX_INCLUDE_DIR}/ucs/version.h")
    file(STRINGS "${UCX_INCLUDE_DIR}/ucs/version.h" _ucx_ver
         REGEX "^#define UCS_VERSION_(MAJOR|MINOR|PATCH)")
    foreach(_def ${_ucx_ver})
        string(REGEX MATCH "UCS_VERSION_(MAJOR|MINOR|PATCH) ([0-9]+)" _ "${_def}")
        if(CMAKE_MATCH_1 STREQUAL "MAJOR") set(_UCX_MAJOR ${CMAKE_MATCH_2}) endif()
        if(CMAKE_MATCH_1 STREQUAL "MINOR") set(_UCX_MINOR ${CMAKE_MATCH_2}) endif()
        if(CMAKE_MATCH_1 STREQUAL "PATCH") set(_UCX_PATCH ${CMAKE_MATCH_2}) endif()
    endforeach()
    set(UCX_VERSION "${_UCX_MAJOR}.${_UCX_MINOR}.${_UCX_PATCH}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(UCX
    REQUIRED_VARS UCX_UCP_LIBRARY UCX_UCT_LIBRARY UCX_UCS_LIBRARY UCX_INCLUDE_DIR
    VERSION_VAR   UCX_VERSION
)

if(UCX_FOUND AND NOT TARGET UCX::ucp)
    add_library(UCX::ucs UNKNOWN IMPORTED)
    set_target_properties(UCX::ucs PROPERTIES
        IMPORTED_LOCATION             "${UCX_UCS_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${UCX_INCLUDE_DIR}"
    )
    add_library(UCX::uct UNKNOWN IMPORTED)
    set_target_properties(UCX::uct PROPERTIES
        IMPORTED_LOCATION "${UCX_UCT_LIBRARY}"
    )
    target_link_libraries(UCX::uct INTERFACE UCX::ucs)

    add_library(UCX::ucp UNKNOWN IMPORTED)
    set_target_properties(UCX::ucp PROPERTIES
        IMPORTED_LOCATION "${UCX_UCP_LIBRARY}"
    )
    target_link_libraries(UCX::ucp INTERFACE UCX::uct UCX::ucs)
endif()

mark_as_advanced(UCX_INCLUDE_DIR UCX_UCP_LIBRARY UCX_UCT_LIBRARY UCX_UCS_LIBRARY)
```

**Recommended Upgrade to CMakeLists.txt**:

```cmake
# Try UCX CMake config package first (UCX >= 1.11)
find_package(ucx CONFIG QUIET)
if(ucx_FOUND)
    message(STATUS "Found UCX via cmake config: ${ucx_VERSION}")
    set(UCX_TARGET ucx::ucp)
else()
    # Fall back to pkg-config
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(UCX QUIET IMPORTED_TARGET ucx)
    endif()
    if(UCX_FOUND)
        message(STATUS "Found UCX via pkg-config: ${UCX_VERSION}")
        set(UCX_TARGET PkgConfig::UCX)
    else()
        # Last resort: custom FindUCX.cmake
        list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
        find_package(UCX REQUIRED)
        set(UCX_TARGET UCX::ucp)
    endif()
endif()

target_link_libraries(axon PUBLIC ${UCX_TARGET} pthread)
```

**Alternatives Considered**:

| Alternative | Why Not Chosen |
|-------------|----------------|
| Hardcode UCX paths | Not portable across environments |
| `FetchContent` to build UCX from source | UCX has complex C build system; takes 10+ minutes; usually pre-installed on HPC systems |
| Require UCX CMake config only | Breaks on UCX < 1.11 and many spack installations |

---

### 2.2 Shared and Static Library Targets

**Decision**: Build both `axon` (shared, `.so`) and `axon_static` (static, `.a`) from the same source list; export both in the CMake install package.

**Rationale**:

Shared libraries are the default for development (faster link, easier `LD_PRELOAD` interception, Python bindings must be shared). Static libraries enable single-binary deployment and avoid runtime library path issues on constrained HPC environments.

```cmake
# Shared sources (define once, use twice)
file(GLOB_RECURSE AXON_SOURCES
    src/core/*.cpp
    src/transport/*.cpp
    src/memory/*.cpp
)

# Shared library
add_library(axon SHARED ${AXON_SOURCES})
set_target_properties(axon PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON
)

# Static library (same sources, different target name)
add_library(axon_static STATIC ${AXON_SOURCES})
set_target_properties(axon_static PROPERTIES
    OUTPUT_NAME axon    # outputs libaxon.a, not libaxon_static.a
    POSITION_INDEPENDENT_CODE ON   # needed if .a is linked into .so
)

# Apply identical interface definitions to both
foreach(_target axon axon_static)
    target_include_directories(${_target}
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:include>
        PRIVATE ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(${_target} PUBLIC ${UCX_TARGET} pthread)
    target_compile_features(${_target} PUBLIC cxx_std_20)
    target_compile_options(${_target} PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<CONFIG:Release>:-O3 -march=native -DNDEBUG>
        $<$<CONFIG:Debug>:-O0 -g3 -fsanitize=address,undefined>
    )
endforeach()

# Install both
install(TARGETS axon axon_static EXPORT axonTargets
    LIBRARY  DESTINATION lib
    ARCHIVE  DESTINATION lib
    RUNTIME  DESTINATION bin
)

# CMake package export
install(EXPORT axonTargets
    FILE      axonConfig.cmake
    NAMESPACE axon::
    DESTINATION lib/cmake/axon
)
install(FILES cmake/axonConfigVersion.cmake DESTINATION lib/cmake/axon)

# Version file
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/axonConfigVersion.cmake"
    VERSION       ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
```

**Link-Time Optimization (LTO)**:

For release builds, enable LTO to allow the linker to inline across the library boundary:

```cmake
include(CheckIPOSupported)
check_ipo_supported(RESULT lto_supported OUTPUT lto_error)
if(lto_supported)
    set_target_properties(axon PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
    )
endif()
```

---

### 2.3 Google Test and Google Benchmark Integration

**Decision**: Use `FetchContent` to pull GoogleTest and GoogleBenchmark at configure time if they are not found as system packages; never bundle vendored copies.

**Rationale**:

`FetchContent` downloads at configure time (once), caches the result, and integrates with CMake's dependency graph. It is preferred over `add_subdirectory` with a git submodule because it does not require developers to run `git submodule update`.

```cmake
# tests/CMakeLists.txt
include(FetchContent)

# --- GoogleTest ---
find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
        GIT_SHALLOW    TRUE
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)   # don't pollute system install
    set(BUILD_GMOCK   ON  CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# --- GoogleBenchmark ---
find_package(benchmark CONFIG QUIET)
if(NOT benchmark_FOUND)
    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.8.4
        GIT_SHALLOW    TRUE
    )
    set(BENCHMARK_ENABLE_TESTING        OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL        OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_DOWNLOAD_DEPENDENCIES ON  CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googlebenchmark)
endif()

# --- Unit Tests ---
add_executable(axon_tests
    unit/test_status.cpp
    unit/test_memory.cpp
    unit/test_future.cpp
    unit/test_worker.cpp
    unit/test_endpoint.cpp
    unit/test_tag_matching.cpp
)
target_link_libraries(axon_tests PRIVATE
    axon
    GTest::gtest_main
    GTest::gmock
)
target_compile_options(axon_tests PRIVATE -fsanitize=address,undefined)
target_link_options(axon_tests PRIVATE -fsanitize=address,undefined)

include(GoogleTest)
gtest_discover_tests(axon_tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    PROPERTIES TIMEOUT 30
)

# --- Benchmarks ---
add_executable(axon_benchmarks
    bench/bench_tag_send.cpp
    bench/bench_memory_registration.cpp
    bench/bench_future.cpp
)
target_link_libraries(axon_benchmarks PRIVATE
    axon
    benchmark::benchmark_main
)
target_compile_options(axon_benchmarks PRIVATE -O3 -march=native -DNDEBUG)
```

**CTest Integration**:

```cmake
# In root CMakeLists.txt
if(AXON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

Run all tests via `ctest --output-on-failure -j$(nproc)`.

**Sanitizer Presets** (CMakePresets.json):

```json
{
    "configurePresets": [
        {
            "name": "asan",
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "Debug",
                "CMAKE_CXX_FLAGS": "-fsanitize=address,undefined -fno-omit-frame-pointer"
            }
        },
        {
            "name": "tsan",
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "Debug",
                "CMAKE_CXX_FLAGS": "-fsanitize=thread -fno-omit-frame-pointer"
            }
        }
    ]
}
```

---

### 2.4 spdlog Integration

**Decision**: Integrate spdlog as an optional dependency; guard all log calls with compile-time severity macros so spdlog can be completely compiled out in production builds.

**Rationale**:

spdlog is a header-only (or compiled) fast logging library with asynchronous backends. It adds ~0 ns overhead when disabled at compile time. The key decision is whether to use it as header-only (`spdlog::spdlog`) or compiled (`spdlog::spdlog_header_only`) — the compiled version avoids template re-instantiation across TUs and is recommended for libraries.

```cmake
# In root CMakeLists.txt
option(AXON_ENABLE_LOGGING "Enable spdlog logging (disable for zero-overhead hot path)" ON)

if(AXON_ENABLE_LOGGING)
    find_package(spdlog CONFIG QUIET)
    if(NOT spdlog_FOUND)
        FetchContent_Declare(
            spdlog
            GIT_REPOSITORY https://github.com/gabime/spdlog.git
            GIT_TAG        v1.13.0
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(spdlog)
    endif()
    target_link_libraries(axon PRIVATE spdlog::spdlog)
    target_compile_definitions(axon PRIVATE AXON_LOGGING_ENABLED)
endif()
```

**Usage Pattern** (in library source):

```cpp
// src/core/logging.h  (internal header)
#ifdef AXON_LOGGING_ENABLED
#  include <spdlog/spdlog.h>
#  define AXON_LOG_DEBUG(...)  spdlog::debug(__VA_ARGS__)
#  define AXON_LOG_INFO(...)   spdlog::info(__VA_ARGS__)
#  define AXON_LOG_WARN(...)   spdlog::warn(__VA_ARGS__)
#  define AXON_LOG_ERROR(...)  spdlog::error(__VA_ARGS__)
#else
#  define AXON_LOG_DEBUG(...)  do {} while(0)
#  define AXON_LOG_INFO(...)   do {} while(0)
#  define AXON_LOG_WARN(...)   do {} while(0)
#  define AXON_LOG_ERROR(...)  do {} while(0)
#endif
```

**Hot-Path Warning**: Never place logging calls on the critical send/recv path without a compile-time guard. Even a no-op function call on the critical path can prevent inlining and add 5-20 ns due to instruction cache pollution.

---

## 3. Error Handling Patterns

### 3.1 Status/ErrorCode Pattern vs Exceptions

**Decision**: Use the `Status` + `ErrorCode` pattern for all synchronous and asynchronous APIs; provide `throw_if_error()` as an opt-in exception bridge for users who prefer exceptions.

**Rationale**:

In high-performance network code, exceptions are inappropriate on the critical path for two reasons:

1. **Performance**: Exception handling involves DWARF table lookups and dynamic allocation for the exception object. On the error path this is acceptable, but the compiler may add guard code that affects the happy path even when no exception is thrown (`-fexceptions` overhead).

2. **Async Propagation**: Exceptions cannot propagate across `ucp_worker_progress` callbacks. The callback is called from inside UCX's stack, not from within a user `try/catch` block. Attempting to throw from a callback causes undefined behavior.

The `Status` type design (from this project's `common.h`) achieves the right balance:

```cpp
// common.h (already implemented in this project)

class Status {
public:
    // Lightweight: just an ErrorCode enum + optional string
    // Size: 1 int + sizeof(std::string) = ~32 bytes
    // On success path: just check ok() == true; no heap allocation, no branching overhead

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] bool in_progress() const noexcept;
    [[nodiscard]] ErrorCode code() const noexcept;
    [[nodiscard]] const std::string& message() const noexcept;

    // Bridge to std::system_error for exception users
    void throw_if_error() const;

    // Bridge to std::error_code for generic error-handling code
    [[nodiscard]] std::error_code error_code() const noexcept;

    static Status OK() noexcept { return {}; }
};
```

**Hot-Path Optimization**:

For the send/recv hot path, the `Status` should be returned by value and the check should be a single comparison:

```cpp
// Optimized status check pattern
Status send_result = ep->tag_send(buf, len, tag).wait();
if (!send_result.ok()) [[unlikely]] {
    handle_error(send_result);
}
```

The `[[unlikely]]` attribute (C++20) hints to the branch predictor that errors are rare, allowing the compiler to place the error handling code out-of-line and keep the hot path branch-prediction friendly.

**Status Propagation in Futures**:

```cpp
// The Future<T> carries Status internally
// On success: future.get() returns T
// On error: future.get() either throws (if configured) or the status is accessible

template <typename T>
T Future<T>::get() {
    // Block until ready
    while (!ready()) {
        worker_->progress();
    }
    if (!status().ok()) {
        status().throw_if_error();  // only throws if configured
        // or: throw std::system_error(status().error_code());
    }
    return extract_value();
}
```

**Alternatives Considered**:

| Alternative | Why Not Chosen |
|-------------|----------------|
| `std::expected<T, Status>` (C++23) | C++23 not available on all target compilers yet; Status already achieves same semantics |
| Exceptions only | Cannot propagate across UCX callbacks; -fexceptions overhead on hot path |
| Return codes (int) | Not type-safe; easy to ignore; no message attachment |
| `outcome::result<T>` | External dependency; overkill when our Status is already minimal |

---

### 3.2 std::error_code Integration

**Decision**: Implement a custom `std::error_category` for `axon::ErrorCode`; register via `is_error_code_enum` specialization to enable implicit conversion.

**Rationale**:

`std::error_code` integration allows axon errors to work with generic error-handling code (logging frameworks, standard library I/O error handling, Asio, etc.) without any conversion layer.

```cpp
// common.h (already implemented in this project)

// 1. Define the category class
class AXONErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override {
        return "axon";
    }

    std::string message(int ev) const override {
        switch (static_cast<ErrorCode>(ev)) {
            case ErrorCode::kSuccess:           return "success";
            case ErrorCode::kInProgress:        return "operation in progress";
            case ErrorCode::kCanceled:          return "operation canceled";
            case ErrorCode::kConnectionRefused: return "connection refused";
            case ErrorCode::kTransportError:    return "transport error";
            case ErrorCode::kOutOfMemory:       return "out of memory";
            // ... all cases ...
            default:                            return "unknown error";
        }
    }

    // Optional: map axon errors to equivalent std::errc conditions
    std::error_condition default_error_condition(int ev) const noexcept override {
        switch (static_cast<ErrorCode>(ev)) {
            case ErrorCode::kConnectionRefused:
                return std::make_error_condition(std::errc::connection_refused);
            case ErrorCode::kOutOfMemory:
                return std::make_error_condition(std::errc::not_enough_memory);
            case ErrorCode::kTimeout:
                return std::make_error_condition(std::errc::timed_out);
            default:
                return std::error_condition(ev, *this);
        }
    }
};

// 2. Singleton accessor
const std::error_category& axon_category() noexcept {
    static AXONErrorCategory cat;
    return cat;
}

// 3. Factory function
inline std::error_code make_error_code(ErrorCode ec) noexcept {
    return {static_cast<int>(ec), axon_category()};
}
```

```cpp
// namespace std (in common.h)
namespace std {
    template<> struct is_error_code_enum<axon::ErrorCode> : true_type {};
}
// This enables: std::error_code ec = axon::ErrorCode::kConnectionRefused;
```

**Usage in error-generic code**:

```cpp
// Works with any code that accepts std::error_code:
std::error_code ec = status.error_code();
if (ec == std::errc::connection_refused) { /* ... */ }

// Works with std::system_error:
throw std::system_error(ec, "tag_send failed");

// Works with Asio error handling:
asio::post(executor, [ec]() { handle_error(ec); });
```

---

### 3.3 Zero-Overhead Error Paths

**Decision**: Use `[[nodiscard]]` on `Status` and `Future<T>` returns; use `[[unlikely]]` on error branches; store error strings lazily (only allocate the message string when an error actually occurs).

**Rationale**:

Three techniques eliminate overhead on the non-error path:

**1. `[[nodiscard]]` to catch ignored errors at compile time**:

```cpp
[[nodiscard]] Status tag_send(const void* buf, size_t len, Tag tag);
// Warning if caller does not check the returned Status
```

**2. Branch predictor hints**:

```cpp
// Compiler places error handling code out-of-line
// and optimizes the fall-through (success) path
if (!status.ok()) [[unlikely]] {
    log_and_cleanup(status);
    return;
}
// hot path continues here inline
```

**3. Lazy string allocation for error messages**:

```cpp
// On the success path: Status is trivially constructable, no allocation
// Status::OK() = { code_ = kSuccess, message_ = "" (empty, no heap) }
//
// On the error path: allocate the string only if there's a message
Status ucx_to_axon_status(ucs_status_t s) {
    if (s == UCS_OK) return Status::OK();   // zero allocation

    // Only here do we pay the string construction cost
    return Status{
        ucx_to_error_code(s),
        std::string(ucs_status_string(s))   // heap alloc, but only on error
    };
}
```

**4. `ErrorCode` comparison without virtual dispatch**:

Since `ErrorCode` is a plain `enum class`, comparison is a single integer comparison — no virtual calls, no heap, no exceptions:

```cpp
if (status.code() == ErrorCode::kInProgress) {
    // Single integer comparison; perfectly predicted by CPU
}
```

**5. `std::string` small-string optimization (SSO)**:

Most error messages are short (< 15 bytes). GCC/Clang's `std::string` SSO stores strings up to 15 characters inline without heap allocation. Keep `ErrorCode` message strings short to take advantage of SSO.

---

## 4. Future/Promise Pattern for UCX Async Operations

### 4.1 Wrapping UCX Requests into C++ Futures

**Decision**: Implement a custom `Future<T>` that wraps a UCX request with a shared state object; the shared state is allocated from a per-worker object pool to avoid `malloc`; completion callbacks resolve the shared state.

**Rationale**:

`std::future` / `std::promise` are unsuitable because:
- `std::promise::set_value` acquires a mutex (unacceptable from a UCX callback).
- `std::future::get` uses `std::condition_variable::wait` which calls into the OS scheduler.
- Neither type is aware of the UCX progress engine; polling via `future.get()` would block without driving progress.

Our custom `Future<T>` solves these problems:

```
                    UCX Operation
                         |
                         v
              ucp_tag_send_nbx(... &params)
                         |
               +---------+---------+
               |                   |
            req == NULL         req pending
          (immediate)            (async)
               |                   |
               v                   v
        SharedState::           UCX request
        set_result(OK)          stored in
               |                UCX pool
               v                   |
         Future<void>          ucp_worker_progress()
         already ready              |
                                    v
                              send_callback()
                                    |
                                    v
                           SharedState::set_result()
                                    |
                                    v
                             Future<void> ready
```

**SharedState Design** (the internal shared state between Future and the completion callback):

```cpp
// Internal, not exposed in public API
template <typename T>
struct SharedState {
    // Written by callback (on worker thread)
    // Read by Future::ready() / Future::get() (potentially any thread)
    std::atomic<bool>         ready{false};
    Status                    status;
    T                         value;          // not present for void specialization
    std::function<void(Status, T)> callback;  // optional, set via on_complete()

    void set_result(Status s, T v) {
        status = std::move(s);
        value  = std::move(v);
        if (callback) callback(status, value);
        ready.store(true, std::memory_order_release);
    }
};

// Future<void> specialization (most common for sends)
template <>
struct SharedState<void> {
    std::atomic<bool>             ready{false};
    Status                        status;
    std::function<void(Status)>   callback;

    void set_result(Status s) {
        status = std::move(s);
        if (callback) callback(status);
        ready.store(true, std::memory_order_release);
    }
};
```

**Future<T> polling** (the non-blocking check used in progress loops):

```cpp
template <typename T>
bool Future<T>::ready() const noexcept {
    return impl_->state->ready.load(std::memory_order_acquire);
}

template <typename T>
T Future<T>::get() {
    // Drive progress until ready
    while (!ready()) {
        impl_->worker->progress();
    }
    impl_->state->status.throw_if_error();
    if constexpr (!std::is_void_v<T>) {
        return std::move(impl_->state->value);
    }
}
```

**Explicit template instantiations** (avoid header-only template bloat):

```cpp
// future.cpp
template class Future<void>;
template class Future<size_t>;
template class Future<std::pair<size_t, Tag>>;
template class Future<uint64_t>;

// In future.h (already in this project):
extern template class Future<void>;
extern template class Future<size_t>;
extern template class Future<std::pair<size_t, Tag>>;
```

---

### 4.2 Avoiding Heap Allocation on the Fast Path

**Decision**: Allocate `SharedState` from a per-worker lock-free free list (object pool); use UCX's custom request memory for inline state when possible.

**Rationale**:

For a high-throughput library processing millions of messages per second, a `malloc`/`free` on every send/recv adds 50-200 ns and causes memory allocator contention. The fast-path allocation budget is zero.

**Two-tier strategy**:

**Tier 1: Inline in UCX request** (for state that only needs to exist during the request):

UCX allocates request objects from its own internal pool. We piggyback our state inline at the beginning:

```cpp
// Registered at ucp_init() time: request_size = sizeof(InlineRequestState)
struct InlineRequestState {
    SharedState<void>* shared;  // pointer to the SharedState (tier 2)
    // We cannot put SharedState inline here because Future<T> needs to
    // outlive the UCX request (which is freed in the callback)
};
```

**Tier 2: Object pool for SharedState**:

```cpp
template <typename T>
class SharedStatePool {
    // Lock-free free list using Treiber stack
    struct Node {
        SharedState<T> state;
        std::atomic<Node*> next{nullptr};
    };

    std::atomic<Node*> head_{nullptr};
    std::vector<std::unique_ptr<Node>> backing_store_;  // prevents deallocation

public:
    SharedState<T>* acquire() {
        Node* node = head_.load(std::memory_order_acquire);
        while (node) {
            if (head_.compare_exchange_weak(node, node->next.load(),
                                            std::memory_order_acq_rel)) {
                node->state = SharedState<T>{};  // reset to clean state
                return &node->state;
            }
        }
        // Pool exhausted: allocate a new node (rare)
        auto& n = backing_store_.emplace_back(std::make_unique<Node>());
        return &n->state;
    }

    void release(SharedState<T>* state) {
        Node* node = reinterpret_cast<Node*>(
            reinterpret_cast<char*>(state) - offsetof(Node, state));
        Node* old_head = head_.load(std::memory_order_relaxed);
        do {
            node->next.store(old_head, std::memory_order_relaxed);
        } while (!head_.compare_exchange_weak(old_head, node,
                                               std::memory_order_acq_rel));
    }
};
```

**Immediate-completion path** (zero allocations):

```cpp
Future<void> Endpoint::tag_send(const void* buf, size_t len, Tag tag) {
    // Acquire a SharedState from the pool (no malloc)
    auto* state = worker_->impl_->send_pool.acquire();

    ucp_request_param_t params = {};
    params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK
                        | UCP_OP_ATTR_FIELD_USER_DATA;
    params.cb.send      = [](void* req, ucs_status_t s, void* ud) {
        auto* st = static_cast<SharedState<void>*>(ud);
        st->set_result(ucx_to_axon_status(s));
        // Release back to pool is done by Future destructor
        ucp_request_free(req);
    };
    params.user_data = state;

    void* ucx_req = ucp_tag_send_nbx(impl_->ep, buf, len, tag, &params);

    if (ucx_req == NULL) {
        // Immediate: no UCX request allocated, callback not called
        state->set_result(Status::OK());
    } else if (UCS_PTR_IS_ERR(ucx_req)) {
        state->set_result(ucx_to_axon_status(UCS_PTR_STATUS(ucx_req)));
    }
    // else: pending, callback will call set_result

    return Future<void>{state, worker_};
}
```

**Memory layout summary**:

| Object | Allocation | Lifetime |
|--------|-----------|---------|
| UCX request block | UCX internal pool | Duration of async op |
| `SharedState<T>` | Worker-local pool | Until Future is destroyed |
| `std::function` callback | Inline SSO or heap | Until `on_complete` fires |

---

### 4.3 Callback Chaining Patterns

**Decision**: Implement `Future<T>::then()` using type-erased continuations stored inline in `SharedState`; chain at most one continuation per future (more requires `wait_all`/`wait_any`).

**Rationale**:

Callback chaining allows composing async operations without blocking:

```cpp
// Send, then when complete, post the next receive
ep->tag_send(buf, len, tag)
  .then([ep, recv_buf](Status s) -> Future<std::pair<size_t, Tag>> {
      if (!s.ok()) throw std::system_error(s.error_code());
      return ep->tag_recv(recv_buf, max_len, reply_tag);
  })
  .on_complete([](Status s, std::pair<size_t, Tag> result) {
      process_reply(result.first, result.second);
  });
```

**`then()` Implementation**:

```cpp
template <typename T>
template <typename Func>
auto Future<T>::then(Func&& func) -> Future<std::invoke_result_t<Func, T>> {
    using U = std::invoke_result_t<Func, T>;

    // Allocate the next future's SharedState from pool
    auto* next_state = worker_->impl_->template get_pool<U>().acquire();
    auto* worker     = worker_;
    auto  f          = std::forward<Func>(func);

    // Install a continuation in this future's SharedState
    impl_->state->callback = [next_state, worker, f = std::move(f)](
                                  Status s, T value) mutable {
        if (!s.ok()) {
            next_state->set_result(s);
            return;
        }
        // Execute the user function and resolve the next future
        if constexpr (std::is_same_v<U, void>) {
            try { f(std::move(value)); next_state->set_result(Status::OK()); }
            catch (...) { next_state->set_result(ErrorCode::kInternalError); }
        } else if constexpr (is_future<U>::value) {
            // func returns a Future: chain through it
            auto inner = f(std::move(value));
            inner.on_complete([next_state](Status s2, typename U::value_type v) {
                next_state->set_result(s2, std::move(v));
            });
        } else {
            try {
                auto result = f(std::move(value));
                next_state->set_result(Status::OK(), std::move(result));
            } catch (...) {
                next_state->set_result(ErrorCode::kInternalError);
            }
        }
    };

    return Future<U>{next_state, worker};
}
```

**`wait_all` Pattern** (batch completion):

```cpp
// Idiomatic usage for pipelining N sends
std::vector<Future<void>> sends;
sends.reserve(N);
for (auto& msg : messages) {
    sends.push_back(ep->tag_send(msg.data(), msg.size(), tag));
}

// Drive progress until all complete
axon::Status result = axon::wait_all(sends);
if (!result.ok()) { /* first error */ }
```

The `wait_all` implementation polls `worker.progress()` and checks each future until all are ready, avoiding any blocking OS calls:

```cpp
template <typename T>
Status wait_all(std::vector<Future<T>>& futures,
                std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    Status first_error;

    while (true) {
        bool all_done = true;
        for (auto& f : futures) {
            if (!f.ready()) {
                all_done = false;
            } else if (!first_error.ok() && !f.status().ok()) {
                first_error = f.status();
            }
        }
        if (all_done) break;

        // Drive progress; if nothing to do, brief wait
        if (!f.request()->worker()->progress()) {
            if (timeout.count() >= 0 &&
                std::chrono::steady_clock::now() > deadline) {
                return ErrorCode::kTimeout;
            }
            f.request()->worker()->wait(std::chrono::milliseconds{1});
        }
    }
    return first_error;
}
```

**Alternatives Considered**:

| Alternative | Why Not Chosen |
|-------------|----------------|
| `std::future` + `std::async` | Thread-per-operation; no UCX progress driving; not suitable |
| Coroutines (`co_await`) | Excellent fit for user-facing API; complex to implement correctly with UCX progress; deferred to v2 |
| Reactive streams (RxCPP) | Large dependency; significant overhead; overkill for point-to-point transport |
| Folly `SemiFuture` | Heavy external dependency; Facebook-specific; build system complexity on HPC clusters |

**Note on C++20 Coroutines**:

C++20 coroutines are the ideal long-term API for this pattern. A `Future<T>` that supports `co_await` would allow:

```cpp
co_await ep->tag_send(buf, len, tag);
auto [bytes, tag] = co_await ep->tag_recv(recv_buf, max_len, any_tag);
```

The UCX progress would be driven inside the coroutine scheduler's `await_suspend`. This is the recommended direction for v2.0, but requires a coroutine scheduler that understands UCX progress (either a dedicated progress thread or integration with an event loop like Asio or liburing).

---

## Summary: Key Decisions at a Glance

| Topic | Decision | Key Reason |
|-------|----------|------------|
| Worker thread mode | `UCS_THREAD_MODE_SINGLE`, 1 worker per thread | No internal locking; linear scaling |
| Cross-thread submission | Lock-free MPSC queue + eventfd | Correct without shared-lock on worker |
| Endpoint connection | `UCP_ERR_HANDLING_MODE_PEER` always | Silent loss prevention on peer failure |
| Server accept | `ucp_listener_create` + connection handler | No external bootstrap needed |
| Tag API generation | `ucp_tag_send_nbx` / `ucp_tag_recv_nbx` | Only API receiving new features |
| RMA consistency | `ucp_ep_flush_nbx` after puts | Required for remote visibility |
| Memory registration | LRU interval tree cache | 10-100 us cost amortized; >95% hit rate |
| Progress engine | Hybrid spin + `ucp_worker_wait` | CPU efficiency without latency sacrifice |
| Completion callbacks | `user_data` in `ucp_request_param_t` | Zero extra allocation on fast path |
| FindUCX | pkg-config with custom .cmake fallback | Handles spack/non-standard installs |
| Library targets | Both shared + static from same sources | Deployment flexibility |
| Test framework | GoogleTest + FetchContent | HPC standard; no submodule required |
| Error handling | `Status` + `ErrorCode` + `std::error_code` | No exceptions across callbacks; generic interop |
| Future pattern | Custom `Future<T>` with pool-allocated SharedState | No malloc per operation; UCX-progress-aware |
| Callback chaining | `.then()` with type-erased continuations | Composable without blocking |

---

## References

- [OpenUCX API Reference](https://openucx.github.io/ucx/api/)
- [OpenUCX FAQ](https://openucx.readthedocs.io/en/master/faq.html)
- [UCX Github: examples/ucp_hello_world.c](https://github.com/openucx/ucx/blob/master/examples/ucp_hello_world.c)
- [UCX Github: test/gtest/ucp/test_ucp_tag.cc](https://github.com/openucx/ucx/blob/master/test/gtest/ucp/test_ucp_tag.cc)
- [UCX Thread Safety Wiki](https://github.com/openucx/ucx/wiki/Thread-safety)
- [UCX Memory Management Wiki](https://github.com/openucx/ucx/wiki/UCX-Memory-management)
- [UCX NVIDIA GPU Support Wiki](https://github.com/openucx/ucx/wiki/NVIDIA-GPU-Support)
- [CMake FetchContent Documentation](https://cmake.org/cmake/help/latest/module/FetchContent.html)
- [GoogleTest CMake Integration](https://google.github.io/googletest/quickstart-cmake.html)
- [spdlog GitHub](https://github.com/gabime/spdlog)
- [P. Geoffray et al. — UCX: An Open Source Framework for HPC Network APIs (2015)](https://doi.org/10.1109/HOTI.2015.13)
- [std::error_code (cppreference)](https://en.cppreference.com/w/cpp/error/error_code)
