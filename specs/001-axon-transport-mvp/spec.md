# Feature Specification: AXON High-Performance Transport Library MVP

**Feature Branch**: `001-axon-transport-mvp`
**Created**: 2026-03-04
**Status**: Draft
**Input**: User description: "High-performance AXON transport library MVP based on UCX, providing C++ API for point-to-point data transfer (1KB-1GB) over RDMA and TCP, targeting AI distributed training, KV Cache inference transfer, and HPC scenarios."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Point-to-Point Tag-Matched Messaging (Priority: P1)

As an AI infrastructure engineer, I want to send and receive data between two nodes using tag-matched messaging, so that I can build gradient synchronization and KV Cache transfer pipelines with precise message routing.

**Why this priority**: Tag-matched send/recv is the fundamental communication primitive. Every other feature depends on having a working, performant message passing layer. Without this, the library has zero value.

**Independent Test**: Can be fully tested by running a ping-pong benchmark between two processes on the same or different hosts, verifying correct data delivery and measuring latency/throughput.

**Acceptance Scenarios**:

1. **Given** two processes connected via the library, **When** process A sends a 4KB buffer with tag=42, **Then** process B receives exactly the same data by matching tag=42, with zero data corruption.
2. **Given** two processes on hosts connected by 200Gbps InfiniBand, **When** process A sends a 1GB buffer, **Then** process B receives it with throughput exceeding 92% of link bandwidth (~23 GB/s).
3. **Given** two processes on hosts connected by 200Gbps InfiniBand, **When** process A sends a 1KB buffer in a ping-pong pattern, **Then** round-trip latency is below 6 microseconds (3us one-way).
4. **Given** process B posts a receive with tag=100, **When** process A sends with tag=200, **Then** process B does not receive the mismatched message and continues waiting.
5. **Given** a sender and receiver, **When** the sender issues a non-blocking send returning a Future, **Then** the caller can poll or wait on the Future and receive a completion notification without blocking the progress engine.

---

### User Story 2 - Connection Lifecycle Management (Priority: P1)

As a distributed system developer, I want to establish, use, and tear down connections to remote peers, so that I can dynamically manage communication topology as nodes join and leave.

**Why this priority**: Connection management is co-equal with messaging — without the ability to connect, listen, and accept, no communication is possible. This is a prerequisite for all other stories.

**Independent Test**: Can be tested by having a server process listen on an address, a client process connect to it, exchanging a small message, and then both sides closing cleanly.

**Acceptance Scenarios**:

1. **Given** a server process calling listen on a network address, **When** a client process connects to that address, **Then** the server receives an accept callback with a usable endpoint within 100ms.
2. **Given** an established connection, **When** one side calls close on the endpoint, **Then** the remote side is notified and both sides release resources without memory leaks.
3. **Given** a client process, **When** it attempts to connect to a non-existent address, **Then** the library returns a timeout error within the configured timeout period (default 10 seconds).
4. **Given** a server listening on a port, **When** 64 clients connect simultaneously, **Then** all connections are established successfully and can exchange messages independently.

---

### User Story 3 - One-Sided RDMA Operations (Priority: P2)

As an HPC application developer, I want to perform RDMA put/get operations to directly read from or write to remote memory, so that I can implement low-latency data access patterns without involving the remote CPU.

**Why this priority**: RDMA one-sided operations enable advanced use cases (remote memory access for parameter servers, KV Cache direct placement) but require working connections and memory registration from P1 stories.

**Independent Test**: Can be tested by having two processes each register a memory region, exchange remote keys, and then one process writes to the other's memory via put, followed by a verification read via get.

**Acceptance Scenarios**:

1. **Given** two connected processes where process B has registered a memory region and shared its remote key, **When** process A performs a put of 1MB to process B's remote address, **Then** process B can read the correct data from its local memory after A calls flush.
2. **Given** process B has data in a registered memory region, **When** process A performs a get of that region, **Then** process A's local buffer contains an exact copy of process B's data.
3. **Given** a registered memory region, **When** the owning process retrieves its remote key, **Then** the key can be serialized, sent to a peer, and used to perform remote operations.

---

### User Story 4 - Asynchronous Operation Model (Priority: P2)

As a framework developer, I want all communication operations to support asynchronous completion via Futures, so that I can overlap communication with computation and build efficient pipeline schedules.

**Why this priority**: Async operations are essential for production use cases where communication-computation overlap determines overall system efficiency. However, basic synchronous send/recv (P1) provides initial value.

**Independent Test**: Can be tested by issuing multiple non-blocking sends, collecting their Futures, performing local computation, then waiting on all Futures and verifying completion.

**Acceptance Scenarios**:

1. **Given** an endpoint, **When** the user issues a tag_send, **Then** the call returns immediately with a Future object without blocking.
2. **Given** a Future from a completed send, **When** the user calls get(), **Then** the result is returned immediately without additional blocking.
3. **Given** 10 outstanding Futures from concurrent sends, **When** the user calls wait_all(), **Then** all operations complete and the user can inspect each result.
4. **Given** a Future, **When** the user registers a completion callback via then(), **Then** the callback is invoked when the operation completes, with the result or error status.

---

### User Story 5 - Memory Registration and Management (Priority: P2)

As a performance-conscious developer, I want to pre-register memory buffers for zero-copy transfer, so that I can eliminate per-message registration overhead on hot paths and achieve maximum throughput for large messages.

**Why this priority**: Memory registration is critical for achieving peak performance with large messages, but the library can function with UCX handling registration transparently for unregistered buffers (at some performance cost).

**Independent Test**: Can be tested by registering a buffer, sending from it repeatedly, and comparing throughput against sending from unregistered buffers.

**Acceptance Scenarios**:

1. **Given** a library context, **When** the user registers a 1GB host memory buffer, **Then** a MemoryRegion handle is returned that can be used for zero-copy send operations.
2. **Given** a MemoryRegion, **When** the user calls remote_key(), **Then** a serializable key is returned that can be shared with peers for RDMA operations.
3. **Given** a registered MemoryRegion, **When** the user deallocates it, **Then** all associated resources (pinned pages, NIC registrations) are released.
4. **Given** a hot loop sending from the same buffer, **When** using a pre-registered MemoryRegion vs an unregistered buffer, **Then** the pre-registered path achieves at least 95% of raw transport throughput.

---

### User Story 6 - Library Configuration and Initialization (Priority: P1)

As a library integrator, I want to configure transport parameters (number of workers, transport selection, timeouts) via a builder API and environment variables, so that I can tune the library for my specific deployment without recompilation.

**Why this priority**: Configuration and initialization are prerequisites for using the library at all. The builder pattern provides a clean, discoverable API for users.

**Independent Test**: Can be tested by creating a Config with various parameter combinations, initializing a Context, and verifying the reported configuration matches.

**Acceptance Scenarios**:

1. **Given** a default Config, **When** a Context is created, **Then** the library initializes with reasonable defaults (auto-detect transport, auto-detect worker count).
2. **Given** a Config with transport set to a specific protocol, **When** a Context is created, **Then** only the specified transport is used.
3. **Given** environment variable overrides (e.g., UCX_TLS=tcp), **When** Config is built with from_env(), **Then** the environment values take precedence over programmatic settings.
4. **Given** an invalid configuration (e.g., negative worker count), **When** Config::build() is called, **Then** a clear error is returned indicating the invalid parameter.

---

### Edge Cases

- What happens when a send is issued to a closed or failed endpoint? The library must return an error status, not crash or hang.
- What happens when a receive buffer is smaller than the incoming message? The library must return a truncation error with the actual message size.
- What happens when the system runs out of memory during memory registration? The library must return a memory allocation error.
- What happens when multiple threads use different Workers concurrently? Each Worker operates independently with no mutual interference (thread-per-worker model).
- What happens when the remote peer crashes mid-transfer? Outstanding Futures must eventually complete with an error status (not hang indefinitely), respecting the configured timeout.
- What happens with zero-length messages? The library must handle zero-length send/recv as valid operations (used as signaling).
- What happens when tag space is exhausted or tags conflict? The 64-bit tag space is managed by the user; the library must correctly match any valid 64-bit tag value.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST provide tag-matched point-to-point messaging (send/recv) supporting arbitrary 64-bit tags and tag masks for flexible message routing.
- **FR-002**: System MUST provide one-sided RDMA operations (put/get/flush) for direct remote memory access without involving the remote CPU.
- **FR-003**: System MUST support message sizes from 0 bytes to 1GB inclusive, automatically selecting optimal transfer protocols (eager for small messages, rendezvous for large messages).
- **FR-004**: System MUST provide connection management capabilities: connect to remote peers by address, listen for incoming connections, accept connections, and close connections cleanly.
- **FR-005**: System MUST support both RDMA (InfiniBand, RoCE) and TCP transport backends, with automatic transport selection based on available hardware.
- **FR-006**: System MUST provide a non-blocking asynchronous operation model where all communication operations return a Future that can be polled, waited on, or chained with callbacks.
- **FR-007**: System MUST provide a synchronous convenience API that blocks until the operation completes, built on top of the async model.
- **FR-008**: System MUST provide a builder-pattern configuration API that supports programmatic configuration, environment variable overrides, and sensible defaults.
- **FR-009**: System MUST support a thread-per-worker model where each Worker is single-threaded and lock-free, enabling linear scaling with core count.
- **FR-010**: System MUST provide memory registration capabilities allowing users to pre-register buffers for zero-copy transfer and obtain remote keys for RDMA operations.
- **FR-011**: System MUST provide structured error handling via a Status type with error codes, supporting both error-code and exception-based programming styles.
- **FR-012**: System MUST support configurable connection timeouts and operation timeouts.
- **FR-013**: System MUST expose a C++ public API with no mandatory external dependencies beyond UCX at runtime.
- **FR-014**: System MUST be buildable as a shared library (.so) and a static library (.a).
- **FR-015**: System MUST provide a progress engine (Worker::progress()) that drives the underlying transport without consuming CPU when idle (event-driven wakeup).

### Key Entities

- **Context**: Top-level library handle owning the transport context and global resources. One per process (or per independent usage domain). Created from a Config.
- **Config**: Immutable configuration object built via a Builder pattern. Holds transport selection, worker count, timeouts, memory pool settings, and arbitrary key-value transport options.
- **Worker**: The progress engine associated with a thread. Owns one transport worker, drives async completions. Each Worker is single-threaded (no internal locking).
- **Endpoint**: A connection to a specific remote peer, created from a Worker. Supports tag messaging, RDMA, and stream operations. Lifecycle: connect → use → close.
- **Listener**: A server-side object that accepts incoming connections on a bound address. Created from a Worker.
- **Future\<T\>**: A handle representing an in-flight asynchronous operation. Supports polling (ready()), blocking (get()/wait()), chaining (then()), and callbacks (on_complete()).
- **MemoryRegion**: A registered memory buffer that enables zero-copy transfer and RDMA. Exposes base address, length, and serializable remote key.
- **Tag**: A 64-bit value used for message matching. Upper 32 bits for context/communicator ID, lower 32 bits for user-defined routing.
- **Status**: An error-handling type combining an error code enum with an optional message string. Supports conversion to std::error_code and throw_if_error().

## Success Criteria *(mandatory)*

### Measurable Outcomes

#### Latency Requirements (RDMA)

- **SC-001a**: Data transfer of 1KB messages achieves one-way latency below 5 microseconds on RDMA networks.
- **SC-001b**: Data transfer of 4KB messages achieves one-way latency below 5 microseconds on RDMA networks.
- **SC-001c**: Library overhead for 1KB-64KB messages is less than 2 microseconds additional latency compared to raw UCX operations.
- **SC-001d**: Library overhead for messages 64KB and larger is less than 5% compared to raw UCX operations (see SC-003).

**Note**: Small messages (<1KB) have higher absolute overhead due to protocol overhead dominating message time. The 2-microsecond overhead budget applies to messages 1KB and above.

#### Throughput Requirements (RDMA)

- **SC-002**: Data transfer of 1GB messages achieves throughput exceeding 92% of the link bandwidth (>23 GB/s on 200Gbps InfiniBand HDR).

#### Library Overhead

- **SC-003**: Library overhead compared to raw UCX operations is less than 5% for messages 64KB and larger, as measured by standardized ping-pong and streaming benchmarks.

#### Transport Compatibility

- **SC-004**: All 6 user stories pass their acceptance scenarios on at least two transport backends (RDMA and TCP).
- **SC-004a**: TCP fallback latency target: 1KB messages achieve one-way latency below 50 microseconds (development/testing only).
- **SC-004b**: Shared memory latency target: 1KB messages achieve one-way latency below 5 microseconds.

#### Usability

- **SC-005**: A new user can write a working send/recv program using the library in under 30 minutes, using only the public headers and example code.

#### Scalability

- **SC-006**: The library handles at least 64 concurrent connections without performance degradation on a single Worker.

#### Error Handling

- **SC-007**: All Futures from failed operations (peer crash, timeout, invalid arguments) complete with an error status within the configured timeout, never hanging indefinitely.

#### GPU Compatibility

- **SC-008**: The library introduces zero GPU SM consumption — all transfers are driven by the host CPU and NIC, consuming no GPU compute resources.

#### Quality Gates

- **SC-009**: Unit test code coverage reaches at least 80% line coverage for core modules (Config, Status, Worker, Endpoint, Future, MemoryRegion).
- **SC-010**: The library builds and passes all tests on Linux x86_64 with the required compiler and build tool versions.

#### Connection Performance

- **SC-011**: First connection establishment completes within 100 milliseconds on RDMA networks.
- **SC-012**: Server listener accepts incoming connections within 100 milliseconds (per US2 acceptance scenario).

### Performance Measurement Methodology

All performance benchmarks MUST use the following methodology:

- **Warmup**: At least 1,000 iterations before measurement to reach steady state
- **Sample Size**: At least 10,000 iterations for latency measurements
- **Percentiles Reported**: p50 (median), p95, p99
- **Test Pattern**: Ping-pong (round-trip) for latency; bulk transfer for throughput
- **Environment**: CPU affinity fixed, Turbo Boost disabled, hyper-threading disabled
- **Message Sizes**: Test at minimum: 64B, 256B, 1KB, 4KB, 64KB, 256KB, 1MB, 64MB, 1GB

**Baseline Comparison**: Performance targets are relative to raw UCX operations using equivalent protocols (e.g., `ucp_tag_send_nbx` vs library `tag_send`).

## Assumptions

- The target platform is Linux x86_64. Other platforms (ARM, macOS) are not in scope for MVP.
- MVP operates on host memory only. GPU memory support (CUDA, Ascend) is deferred to subsequent phases.
- Python bindings are deferred to Phase 2. MVP is C++ only.
- Plugin layer (NCCL/HCCL) is deferred. MVP provides only the core AXON transport.
- Performance targets assume 200Gbps InfiniBand HDR or equivalent RDMA fabric. TCP performance targets are not defined for MVP (TCP serves as a fallback for development/testing).
- Thread safety is provided by the thread-per-worker model (each Worker accessed by exactly one thread). Cross-thread Worker access is undefined behavior.

## Scope Boundaries

**In scope for MVP:**
- Core AXON transport: tag_send, tag_recv, put, get, flush
- Connection management: connect, listen, accept, close
- Async model: Future\<T\> with poll/wait/callback
- Memory registration: register, deregister, remote key
- Config/Context initialization with builder pattern
- Host memory only
- RDMA (IB/RoCE) and TCP transport backends
- C++ API only
- Basic error handling and timeouts
- Unit tests and integration tests
- Performance benchmarks (ping-pong latency, streaming throughput)

**Out of scope for MVP:**
- GPU memory (CUDA, Ascend) — Phase 2
- Python bindings — Phase 2
- NCCL/HCCL Plugin — Phase 2/3
- MemoryPool (slab allocator) — Phase 2
- RegistrationCache (LRU) — Phase 2
- Non-contiguous memory scatter/gather — Phase 2
- Multi-path RDMA aggregation — Phase 3
- Automatic reconnection / fault recovery — Phase 3
- Observability (metrics, tracing) — Phase 3
- Stream operations (ordered byte stream without tags)
- Atomic operations (fetch-and-add, compare-and-swap)

## Dependencies

- **UCX >= 1.14**: The sole runtime dependency. Provides the transport abstraction layer.
- **CMake >= 3.20**: Build system requirement.
- **C++20 compiler (GCC >= 11 or Clang >= 14)**: Language standard requirement.
- **Google Test**: Test framework (build-time dependency only).
- **Google Benchmark**: Performance benchmarking (build-time dependency only).
- **spdlog**: Logging framework (optional, can be disabled).
