# Implementation Research: AXON High-Performance Transport Library MVP

**Date**: 2026-03-04
**Input**: Feature specification, Constitution

---

## Executive Summary

This research document consolidates findings from parallel research by PM, Arch, Dev, and QA roles. Key decisions:

1. **Thread Model**: Per-thread Worker with UCS_THREAD_MODE_SINGLE
2. **Delivery Order**: US6 (Config) → US2+US5 (parallel) → US1+US4 → US3
3. **Hello World MVP**: Tag send/recv achievable by Week 14
4. **Top Risk**: RDMA implementation complexity (mitigated by early spike + fallback)

---

## 1. Architecture Decisions

### 1.1 UCX Worker Patterns

| Decision | Rationale |
|----------|-----------|
| `UCS_THREAD_MODE_SINGLE` | Zero internal locking overhead, linear scaling with cores |
| Event-driven via `event_fd` | Best for asyncio/Python integration |
| Per-thread Worker | UCX worker is not thread-safe; explicit affinity is safer |

**Alternatives considered**:
- Worker pool: More complex, useful when threads >> cores (not MVP)
- Busy polling: Lowest latency, highest CPU (acceptable for benchmarks)

### 1.2 Tag Messaging API

| Decision | Rationale |
|----------|-----------|
| `ucp_tag_send_nbx/recv_nbx` | Non-blocking, auto protocol selection |
| Message thresholds: <8KB eager, 8-256KB eager zcopy, >256KB rendezvous | Optimized for zero-copy on large messages |

### 1.3 RMA Operations

| Decision | Rationale |
|----------|-----------|
| `ucp_put_nbx/get_nbx/flush_nbx` | One-sided, bypasses remote CPU |
| Pre-register + LRU cache | 10-100us registration cost amortized |

---

## 2. C++ API Design Patterns

### 2.1 RAII Wrappers

| UCX Handle | AXON Wrapper | Ownership |
|------------|--------------|-----------|
| `ucp_context_h` | `Context::Impl` | shared_ptr, thread-safe |
| `ucp_worker_h` | `Worker::Impl` | shared_ptr, single-threaded |
| `ucp_ep_h` | `Endpoint::Impl` | shared_ptr, worker-affine |
| `ucp_mem_h` | `MemoryRegion::Impl` | shared_ptr |

### 2.2 Future<T> Pattern

- `Request` wraps low-level `ucs_status_ptr_t`
- `Future<T>` provides typed, composable async results
- Thread-local pool avoids heap allocation on fast path

### 2.3 Config Builder

- Fluent Builder API with `from_env()` support
- Prefix `AXON_` for library options, pass UCX options through

---

## 3. MVP Delivery Strategy

### 3.1 Implementation Order

| Phase | Weeks | Stories | Milestone |
|-------|-------|---------|-----------|
| 1 | 1-4 | US6 | Config & Init |
| 2A | 5-9 | US2 | Connections |
| 2B | 5-9 | US5 | Memory Registration |
| 3 | 10-14 | US1+US4 | **Hello World MVP** |
| 4 | 15-18 | US3 | RDMA Ready |
| 5 | 19-20 | Perf | Production Ready |

**Critical Path**: US6 → US2 → US1 = 14 weeks to MVP value

### 3.2 Hello World Milestone (Week 14)

Two processes exchange 4KB message with tag matching:
```cpp
// Receiver
Listener listener = worker->listen("tcp://0.0.0.0:1234", on_accept);

// Sender
auto ep = worker->connect("tcp://localhost:1234");
ep->tag_send(buf.data(), buf.size(), tag);
```

---

## 4. Risk Assessment

### 4.1 Top 3 Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| RDMA Implementation | 15-20% | HIGH | Week 6-7 spike + fallback to messaging-only |
| Memory Registration Performance | 25-30% | MEDIUM | Benchmark vs NCCL, add MemoryPool in Phase 2 |
| Plugin Integration (HCCL) | 35-40% | MEDIUM-HIGH | Early architecture spike, defer to Phase 2 |

### 4.2 Fallback Strategies

- **RDMA blocked**: Ship US1-US4 (messaging) at Week 14, defer US3 to Phase 2
- **Registration slow**: Relax target to 80% line bandwidth, document limitation
- **HCCL infeasible**: Ship as NVIDIA-only, defer Ascend to Phase 2

---

## 5. Testing Strategy

### 5.1 Unit Tests

| Component | Strategy |
|-----------|----------|
| Config | Test builder/validation without UCX |
| Status | Test error codes/messages standalone |
| Future | Mock Request for callback/then testing |

### 5.2 Integration Tests

| Scenario | Approach |
|----------|----------|
| Tag send/recv | Multi-process via fork/spawn |
| Connection lifecycle | Graceful close, reconnection |
| RDMA put/get | Requires RDMA hardware or SoftRoCE |

### 5.3 Performance Benchmarks

- **Latency**: Ping-pong round-trip, p50/p99/p999 percentiles
- **Throughput**: Bulk transfer, GB/s
- **Statistical significance**: 10,000+ iterations, bootstrap confidence intervals

### 5.4 CI/CD

- **No RDMA**: `UCX_TLS=tcp,self,shm` for CI
- **ASan**: Leak detection on debug builds
- **Coverage**: gcov/lcov, target 80% line coverage

---

## 6. Build System

### 6.1 CMake

- Use `find_package(UCX)` with config mode, fallback to PkgConfig
- Build shared library (.so) by default, static (.a) optional

### 6.2 Dependencies

- UCX >= 1.14 (runtime)
- spdlog (optional logging)
- Google Test (tests)
- Google Benchmark (benchmarks)

---

## 7. Constitution Compliance

| Principle | Status | Evidence |
|-----------|--------|----------|
| I. Requirements-Driven | ✅ PASS | 6 prioritized user stories, quantitative success criteria |
| II. Architecture-First | ✅ PASS | Layered design, UCX abstraction, thread model defined |
| III. Quality-First | ✅ PASS | Status/ErrorCode, RAII, clang-format/tidy planned |
| IV. Test-Driven | ✅ PASS | Google Test + Benchmark, 80% coverage target |
| V. Coordinated Delivery | ✅ PASS | Phased delivery, P1 first, checkpoints defined |

---

## 8. References

- `/specs/001-axon-transport-mvp/spec.md` - Feature specification
- `/include/axon/*.h` - Public API headers
- `/src/transport/ucx_impl_notes.h` - Implementation details
- `/docs/reports/report-arch-design.md` - Architecture decisions
- `/docs/reports/test-strategy.md` - QA test plan
