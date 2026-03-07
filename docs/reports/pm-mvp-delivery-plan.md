# P2P High-Performance Transport Library MVP — Delivery Plan

**Role**: Product Manager (PM)
**Date**: 2026-03-04
**Status**: Final Delivery Strategy
**Audience**: Engineering leadership, project sponsors, technical stakeholders

---

## Executive Summary

This document defines the **optimal delivery order**, **milestone definitions**, and **risk assessment** for the P2P Transport Library MVP. The strategy is based on:

1. **Dependency Analysis**: Each user story's technical prerequisites
2. **Value Delivery**: Minimum viable features that unlock customer value
3. **Risk Mitigation**: Early validation of hardest technical challenges
4. **Timeline Efficiency**: Parallelizable work streams to maximize team throughput

**Key Recommendation**: Deliver in **5 sequential phases** spanning **4 "hello world" milestones**, with a suggested total MVP timeline of **16–20 weeks** for a 6-8 person team.

---

## Part 1: Optimal MVP Delivery Order

### 1.1 Dependency Graph

The six user stories have the following dependency relationships:

```
US6: Config/Context       (NO DEPENDENCIES)
│
├─> US2: Connection       (depends on Config)
│   │
│   ├─> US1: Tag Messaging (depends on Connections)
│   │   └─> US4: Async Model (enhances Messaging)
│   │   │
│   │   └─> US5: Memory Registration (enhances RDMA)
│   │       └─> US3: RDMA Operations (depends on Connections + Memory)
│   │
│   └─> US3: RDMA (depends on Connections + Memory)
│
└─> US5: Memory Reg. (depends on Config)
    └─> US3: RDMA (depends on Connections + Memory)
```

**Critical Observations**:
- **US6 (Config)** is the only zero-dependency story; must be first
- **US2 (Connection)** and **US5 (Memory Registration)** are independent once Config exists
- **US1 (Tag Messaging)** requires US2 but not US5 or US3
- **US4 (Async)** should follow US1's synchronous foundation
- **US3 (RDMA)** is the deepest in the dependency chain (depends on US2 + US5)

**Parallelization Opportunity**: US2 and US5 can proceed in parallel after US6, reducing critical path length.

---

### 1.2 Recommended Delivery Sequence

#### **Phase 1: Foundation (Weeks 1–4)**
**Deliverable**: Library Configuration and Initialization (US6)

| Story | Why First | What | Done When |
|-------|-----------|------|-----------|
| **US6** | Zero dependencies | Config builder, Context init, basic UCX setup | Can create Context with defaults + env var overrides |

**Scope Boundaries**:
- ✅ Config builder pattern with sensible defaults
- ✅ Context initialization with transport auto-detection
- ✅ Environment variable override support
- ✅ Basic error handling (Status type)
- ✅ Worker creation (single-threaded skeleton)
- ❌ No messaging yet
- ❌ No memory registration
- ❌ No connections

**Definition of Done**:
- Config builder compiles and creates valid Context
- Unit tests for Config validation pass (invalid inputs rejected)
- Example code: `auto ctx = Config().set_worker_count(4).build();`

**Rationale**: This foundation is mandatory for every subsequent phase. Validates build system, dependency management (UCX), and basic library structure.

---

#### **Phase 2A: Connection Management (Weeks 5–9, parallel with 2B)**
**Deliverable**: Connection Lifecycle Management (US2)

| Story | Why Now | What | Done When |
|-------|---------|------|-----------|
| **US2** | Depends only on US6 | Listen, accept, connect, close | Can establish 1-to-1 connections |

**Scope Boundaries**:
- ✅ Server listen on address
- ✅ Client connect to address
- ✅ Accept callback invoked
- ✅ Endpoint lifecycle: created → open → closed
- ✅ Connection timeouts (configurable)
- ✅ Error handling (connection refused, timeout)
- ❌ No tag messaging yet
- ❌ No memory registration or RDMA

**Definition of Done**:
- Can run: `server.listen("tcp://0.0.0.0:1234")` → `client.connect("tcp://localhost:1234")`
- Endpoints are created and managed
- Server-side accept callback receives usable Endpoint
- Close() releases resources without leaks
- 64 concurrent connections establish without degradation

**Rationale**: Connection management is co-equal with messaging. Both are needed for the simplest send/recv operation. Provides early validation of endpoint management, address parsing, and UCX transport initialization.

**Deliverable Artifact**:
```cpp
// Minimal viable code after Phase 2A
Listener listener = ctx->worker->listen("tcp://0.0.0.0:1234",
    [](Endpoint& ep) { /* accept callback */ });
Status s = client_worker->connect("tcp://server:1234", ep_out);
```

---

#### **Phase 2B: Memory Registration (Weeks 5–9, parallel with 2A)**
**Deliverable**: Memory Registration and Management (US5)

| Story | Why Now | What | Done When |
|-------|---------|------|-----------|
| **US5** | Depends only on US6 | Register, deregister, remote key | Can register host memory |

**Scope Boundaries**:
- ✅ Register host memory buffer
- ✅ Deregister with cleanup
- ✅ Obtain serializable remote key
- ✅ Memory type detection (host, GPU if available)
- ✅ Pin/unpin pages via UCX
- ❌ Memory pools (deferred to Phase 3)
- ❌ Registration cache (deferred to Phase 3)
- ❌ Scatter/gather (deferred to Phase 3)

**Definition of Done**:
- `MemoryRegion mr = ctx->register_memory(buf, size);`
- `RemoteKey rkey = mr->remote_key();`
- Serialized key can be sent to peer
- Deregister releases pinned pages

**Rationale**: Memory registration is independent of connection setup and required for RDMA (US3). Separating it allows RDMA team to parallelize. Validates UCX memory registration API and pin/unpin mechanics.

**Deliverable Artifact**:
```cpp
auto mr = ctx->register_memory(buffer, 1024*1024,
    MemoryType::kHost);
RemoteKey rkey = mr->remote_key();
```

---

#### **Phase 3: Tag-Matched Messaging (Weeks 10–14)**
**Deliverable**: Point-to-Point Tag-Matched Messaging (US1, sync-only)

| Story | Why Now | What | Done When |
|-------|---------|------|-----------|
| **US1** | Depends on US2 | Blocking tag_send / tag_recv | Can exchange small messages |
| **US4** | Enhanced from US1 | Non-blocking async model via Future | Can collect completions asynchronously |

**Scope Boundaries**:
- ✅ Synchronous tag_send() (blocking until remote receives)
- ✅ Synchronous tag_recv() (blocking until data arrives)
- ✅ Tag matching with 64-bit tags (no masks yet)
- ✅ Message sizes 1 byte to 1GB
- ✅ Automatic eager/rendezvous protocol selection
- ✅ Data corruption detection (CRC in tests)
- ✅ Non-blocking Future<Status> for send/recv
- ✅ Future::get() (blocking), Future::ready() (poll)
- ✅ Future::wait_all() for multiple completions
- ❌ Tag masks (advanced feature, deferred)
- ❌ Active Message (deferred)
- ❌ Stream operations (deferred)

**Definition of Done**:
- Synchronous: `ep->tag_send(buf, size, tag);` blocks until received
- Synchronous: `ep->tag_recv(buf, size, tag);` blocks until data arrives
- Asynchronous: `auto fut = ep->tag_send_nb(buf, size, tag); fut->wait();`
- Ping-pong throughput achieves 90%+ of raw UCX for 1MB+ messages
- Ping-pong latency achieves < 5us for small messages (within 2us of raw)
- Multi-message send/recv without deadlock

**Rationale**: Tag messaging is the **core value proposition**. Once connections and messaging work, the library provides value to customers. Async model (Futures) is the abstraction layer for all non-blocking operations, so must be integrated here.

**Deliverable Artifact**:
```cpp
// Synchronous
ep->tag_send(data, 1024, 42);
ep->tag_recv(data, 1024, 42);

// Asynchronous
auto fut = ep->tag_send_nb(data, 1024, 42);
ep->progress();  // drive completions
auto status = fut->wait();
```

**"Hello World" Milestone**: This phase completes **Milestone 2: "Hello World" Basic Messaging**

---

#### **Phase 4: One-Sided RDMA (Weeks 15–18)**
**Deliverable**: One-Sided RDMA Operations (US3)

| Story | Why Now | What | Done When |
|-------|---------|------|-----------|
| **US3** | Depends on US2 + US5 | put, get, flush | Can write to remote memory without CPU involvement |

**Scope Boundaries**:
- ✅ RDMA put (write to remote memory)
- ✅ RDMA get (read from remote memory)
- ✅ Flush (ensure write completion)
- ✅ Work with MemoryRegions from US5
- ✅ Remote key exchange via messaging
- ✅ One-sided operations (no remote CPU involvement)
- ❌ Atomic operations (deferred)
- ❌ Scatter/gather (deferred)

**Definition of Done**:
- Process A registers memory region, sends remote key to B
- Process B performs put to write 100MB to A's memory
- Process B performs get to read A's memory
- Data verified on both sides
- Put/get throughput > 85% of link bandwidth for 1MB+ messages

**Rationale**: RDMA is the differentiator for high-performance scenarios (parameter servers, KV cache direct placement). However, it requires both connection management and memory registration, so comes last. Early message passing provides customer value; RDMA enhances it.

**Deliverable Artifact**:
```cpp
// Process A: register and expose
auto mr = ctx->register_memory(buffer, size);
auto rkey = mr->remote_key();
ep_to_b->tag_send(rkey.serialize(), ...);

// Process B: receive and use
auto rkey_buf = ep_from_a->tag_recv(...);
RemoteKey rkey = RemoteKey::deserialize(rkey_buf);
ep_from_a->put(local_data, size, remote_addr, rkey);
ep_from_a->flush();
```

---

#### **Phase 5: Performance Optimization & Polish (Weeks 19–20)**
**Deliverable**: Performance targets met, MVP complete

| What | Target | How |
|------|--------|-----|
| Small message latency | < 5us (within 2us of raw UCX) | Tune eager protocol, reduce copies |
| Large message throughput | > 90% of link bandwidth | Verify rendezvous path, zero-copy |
| Connection establishment | < 100ms | Validate endpoint pooling |
| No memory leaks | Verified with ASAN | Full regression test suite |
| 80% code coverage | Mandatory | Hit all unit test gates |

**Scope**:
- ✅ Performance benchmarking and tuning
- ✅ Memory leak verification
- ✅ Error path testing (fault injection)
- ✅ Documentation and examples
- ✅ Final integration test suite
- ❌ Advanced features (pools, caches, scatter/gather)

**Definition of Done**: All acceptance scenarios from spec pass on RDMA + TCP transports.

---

### 1.3 Timeline Summary

| Phase | Duration | Story | Milestone | Cumulative Output |
|-------|----------|-------|-----------|-------------------|
| 1 | 4 weeks | US6 | Setup | Can create Context |
| 2A | 5 weeks (parallel) | US2 | Connections | Can connect 2 processes |
| 2B | 5 weeks (parallel) | US5 | Memory | Can register memory |
| 3 | 5 weeks | US1+US4 | **Messaging** | **"Hello World" MVP** |
| 4 | 4 weeks | US3 | RDMA | Complete P2P library |
| 5 | 2 weeks | Perf/Polish | Release | Ready for production |
| | | | **Total** | **16–20 weeks** |

**Critical Path**: US6 → US2 → US1 (13 weeks baseline)
**Parallel Optimization**: Run US2 + US5 in parallel saves ~5 weeks

---

## Part 2: Minimum Viable Milestone Definitions

### Milestone 1: "Config & Init" (End of Week 4)
**Objective**: Establish library foundation; validate build system and toolchain

**Acceptance**:
- ✅ `Config().set_worker_count(4).build()` succeeds
- ✅ Invalid config rejected with clear error (e.g., negative worker_count)
- ✅ Environment variable `P2P_TRANSPORT=tcp` overrides config
- ✅ CMake build completes on Linux x86_64 with GCC 11+, Clang 14+
- ✅ Unit tests for Config validation pass

**Why This**: Validates that the project structure, dependency management (UCX), and build system work before committing to larger features.

---

### Milestone 2: "Hello World – Connections & Messaging" (End of Week 14)
**Objective**: Library provides customer-visible value; customers can build gradient synchronization or KV Cache transfer pipelines

**Acceptance**: Two processes (local or remote) can exchange 4KB message with tag matching
```cpp
// Receiver process
Listener listener = ctx->worker->listen("tcp://0.0.0.0:1234",
    [](Endpoint& ep) {
        std::vector<uint8_t> buf(4096);
        ep->tag_recv(buf.data(), buf.size(), 42);
        assert(buf[0] == 0xAB);
    });

// Sender process
auto ep = ctx->worker->connect("tcp://localhost:1234");
std::vector<uint8_t> buf(4096, 0xAB);
ep->tag_send(buf.data(), buf.size(), 42);
```

**Metrics**:
- Ping-pong round-trip latency < 20us (on RDMA) for 4KB messages
- Ping-pong throughput > 1GB/s for 64KB messages
- Zero data corruption (verified with CRC32)
- 16 concurrent connections without deadlock

**Why This**: Provides immediate customer value for synchronization-heavy workloads. Customers can prototype gradient/KV Cache pipelines. Validates tag matching, endpoint management, and message ordering.

---

### Milestone 3: "RDMA Ready" (End of Week 18)
**Objective**: Enable low-latency remote memory access; unlock parameter server and advanced inference scenarios

**Acceptance**: Process A writes 1GB to Process B's registered memory via RDMA put
```cpp
// Process A
auto ep = ctx->worker->connect("tcp://process_b:1234");
auto mr = ctx->register_memory(local_data, 1024*1024*1024);
auto rkey = mr->remote_key();
ep->tag_send(rkey.serialize(), ...);  // send remote key

// Process B receives remote key, sends address
// Process A performs put
ep->put(local_data, 1GB, remote_addr, remote_key);
ep->flush();
```

**Metrics**:
- RDMA put throughput > 22 GB/s for 1GB message (on 200Gbps network)
- RDMA get latency < 50us for 1MB message
- Memory registration/deregistration < 10ms for 1GB buffer
- No CPU involvement in data transfer (verified with performance counters)

**Why This**: Completes the core P2P library. Parameter servers can implement remote model reads, KV Cache transfer can use direct placement, training frameworks can optimize communication pipelines.

---

### Milestone 4: "Production Ready" (End of Week 20)
**Objective**: Library meets all success criteria; ready for customer use

**Acceptance**:
- ✅ All 6 user story acceptance scenarios pass
- ✅ No memory leaks (ASAN clean)
- ✅ 80% code coverage achieved
- ✅ Performance targets met (SC-001 to SC-010 from spec)
- ✅ Documentation and examples complete
- ✅ Works on RDMA + TCP + Shared Memory transports
- ✅ Handles 64 concurrent connections
- ✅ All fault injection tests pass (peer crash, network failure, OOM)

**Why This**: Library is feature-complete, performant, reliable, and ready for production deployment.

---

## Part 3: Risk Assessment for MVP

### 3.1 Top 3 Technical Risks

#### **Risk 1: RDMA Implementation Complexity (HIGH)**

**Description**: RDMA put/get (US3) requires memory registration, remote key exchange, and tight integration with UCX's RMA layer. If UCX RMA API is not feature-complete or has undocumented limitations, the implementation could exceed timeline.

**Impact**:
- **If unmitigated**: RDMA phase (Week 15–18) slips 4–8 weeks
- **Business impact**: Parameter server and KV Cache scenarios delayed to Phase 2; messaging-only library ships (partial value)

**Probability**: 15–20% (MEDIUM)
- UCX is mature for tag messaging but RMA is less documented
- GPU memory RMA support is incomplete in some UCX versions

**Fallback Strategy 1** (Preferred):
1. **Week 6–7**: Spike research on UCX RMA API (tag_send/recv to remote_key)
   - Identify UCX version requirements
   - Document limitations (e.g., GPU memory RMA gaps)
   - Validate basic put/get with small messages
2. **Week 8**: Decision gate: proceed if spike succeeds, otherwise pivot to Strategy 2
3. **If blocked**: Implement RDMA layer as thin wrapper over eager tag_send (high CPU cost but feature-complete)

**Fallback Strategy 2** (Contingency):
- Ship US1–US4 (messaging + async) as "MVP" in Week 14
- Defer RDMA (US3) to "Phase 2" (week 20+)
- Customers can still implement parameter servers via tag_send on hot path (performance degrades by 20–40% but still viable)

**Owner**: Tech Lead (Architecture)

---

#### **Risk 2: Memory Registration Performance (MEDIUM)**

**Description**: UCX memory registration (pin pages, register with NIC) can be slow or fail under pressure. If registration becomes a bottleneck or frequently fails, performance targets (SC-002, SC-004) cannot be met.

**Impact**:
- **If unmitigated**: Large message throughput fails to achieve > 90% line bandwidth target
- **Business impact**: Library appears "slow" compared to NCCL; adoption slows
- **Timeline**: 2–4 weeks added to Phase 3–4 for registration caching and optimization

**Probability**: 25–30% (MEDIUM)
- UCX registration is well-tested in NCCL/HPC production, so baseline is solid
- But GPU memory registration can fail if buffers > 256MB or pinned pool is exhausted

**Fallback Strategy 1** (Preferred):
1. **Week 5**: Benchmark UCX registration speed vs. NCCL (establish baseline)
2. **Week 12**: Pre-register frequently-used buffers via MemoryPool (Phase 3)
3. **Week 15**: Add RegistrationCache (LRU) to avoid repeated registration of same buffer
4. **Acceptance**: Achieve > 95% throughput of raw transport for 1MB+ messages

**Fallback Strategy 2** (Contingency):
- Relax performance target for pre-unregistered buffers (e.g., 80% vs 90%)
- Document in release notes: "Pre-register hot buffers for best performance"
- Customers can optimize their code paths (explicit registration)

**Owner**: Performance Engineer

---

#### **Risk 3: Ascend NPU Support / Plugin Integration (MEDIUM–HIGH)**

**Description**: MVP spec assumes "host memory only" but Phase 2 will require HCCL plugin for Ascend NPU memory. If plugin architecture is not designed well in Phase 1, Phase 2 integration becomes hard. Also, if Ascend HCCL API changes or is incompatible with plugin assumptions, integration delays cascade.

**Impact**:
- **If unmitigated**: Ascend support (Phase 2+) slips or requires rework
- **Business impact**: Can't claim "dual-stack NVIDIA+Ascend" in marketing; loses key differentiator
- **Timeline**: 4–6 weeks added to Phase 2 for redesign or compatibility workarounds

**Probability**: 35–40% (MEDIUM–HIGH, due to external dependency on Ascend team)
- HCCL API is proprietary and evolves with Ascend NPU drivers
- Plugin mechanism is unproven in this codebase

**Fallback Strategy 1** (Preferred):
1. **Week 2–3**: Architecture spike: design plugin interface (IPlugin, PluginRegistry) to be decoupled from core P2P
2. **Week 6**: Early engagement with Ascend team: validate plugin assumptions against HCCL API v8.x and v9.x
3. **Week 12–14** (Phase 3): Implement mock HCCL plugin to verify architecture
4. **Decision gate at Week 15**: If HCCL integration looks feasible, proceed to Phase 2; else, document MVP as NVIDIA-only

**Fallback Strategy 2** (Contingency):
- Ship MVP as "host memory + NVIDIA GPU (via CUDA)" only
- Ascend support deferred to Phase 2 with separate project
- Reduces MVP scope but avoids risky external dependency in critical path

**Owner**: Architecture Lead

---

### 3.2 Secondary Risks (Medium-Low Priority)

| Risk | Description | Mitigation |
|------|-------------|-----------|
| **Python Bindings Complexity** | nanobind / GIL release / async interop with CPython event loop | Defer to Phase 2; MVP is C++ only |
| **Connection Pooling** | Establishing 64 concurrent connections may fail due to FD limits or memory pressure | Set ulimit in CI/tests; stress-test early (Week 8) |
| **Transport Selection** | Auto-detecting RDMA vs TCP vs Shared Memory might select suboptimal protocol | Document override via `UCX_TLS` env var; customers can tune |
| **Test Infrastructure** | Multi-process / multi-node testing is harder to debug than single-process unit tests | Invest in pytest fixtures + CI containers (Week 1); use SoftRoCE for RDMA testing |
| **Dependency Fragmentation** | Different teams want different UCX versions (1.14 vs 1.16); ABI compatibility breaks | Test against UCX 1.14+; document min version as 1.14; use feature-checking macros |

---

### 3.3 Risk Monitoring & Escalation

**Weekly Risk Review**:
- Every Friday standup: assess top 3 risks
- Update probability/impact if new data emerges
- Trigger escalation if risk probability exceeds 50% or impact becomes "CRITICAL"

**Escalation Triggers** (→ Emergency meeting + re-plan):
1. **RDMA spike (Week 7) reveals UCX RMA incompatible** with our use case
2. **Registration performance baseline (Week 5) shows > 50% overhead vs. NCCL**
3. **Ascend team signals HCCL API breaking change in next release**
4. **Any team member signals 2+ week slip on critical path**

**Risk Registry** (track in project board):
- Title: "Risk: [Name]"
- Fields: Probability, Impact, Mitigation, Fallback, Owner, Review Date
- Auto-escalate if Probability × Impact > threshold (e.g., > 0.4)

---

## Part 4: Success Criteria & Acceptance

The MVP is **done** when:

### Functionality (All 6 User Stories)
- [ ] **US6**: Config builder creates valid Context; env var overrides work
- [ ] **US2**: Two processes connect and maintain endpoint
- [ ] **US1**: Tag send/recv exchanges data with zero corruption
- [ ] **US4**: Futures return immediately; can wait/poll on completions
- [ ] **US5**: Memory region registered, remote key obtained
- [ ] **US3**: RDMA put/get works with registered memory

### Performance (From Success Criteria)
- [ ] **SC-001**: 1KB message achieves < 5us latency (RDMA)
- [ ] **SC-002**: 1GB message achieves > 92% of link bandwidth (RDMA)
- [ ] **SC-003**: Overhead vs raw UCX < 5% for 64KB+ messages
- [ ] **SC-006**: 64 concurrent connections established without degradation
- [ ] **SC-007**: Failed operations complete with error within timeout, never hang
- [ ] **SC-009**: 80% line coverage, 70% branch coverage

### Quality
- [ ] All unit tests pass (gtest)
- [ ] All integration tests pass (pytest)
- [ ] ASAN clean (no memory errors)
- [ ] TSAN clean (no data races)
- [ ] No performance regressions vs baselines

### Documentation
- [ ] API reference (doxygen)
- [ ] "Hello World" example (send/recv)
- [ ] "RDMA Example" (put/get with remote key)
- [ ] Architecture guide (for future contributors)

---

## Part 5: Resource & Team Allocation

### Suggested Team (6–8 FTE)

| Role | Allocation | Key Responsibilities |
|------|------------|----------------------|
| **Tech Lead / Arch** | 1.0 FTE | Design phases 1–2; risk mitigation (RDMA spike, plugin design) |
| **Core C++ Eng** | 2.0 FTE | Implement US6, US2, US1 (phases 1–3) |
| **RDMA Specialist** | 1.5 FTE | Implement US5, US3 (phases 2B–4); RDMA protocol tuning |
| **Performance Eng** | 1.0 FTE | Benchmarking; performance targets; tuning phase 5 |
| **QA / Test Eng** | 1.0 FTE | Test infrastructure; CI/CD; fault injection |
| **PM / Technical Writer** | 0.5 FTE | Spec clarity; milestone tracking; docs |
| **GPU/Ascend Liaison** (optional) | 0.5 FTE | Ascend HCCL compatibility research (risk mitigation) |

### Dependency on External Teams
- **UCX Maintainers**: For RMA API clarification (non-critical path; can async-loop)
- **Ascend Team**: For HCCL API details (Phase 2; not blocking MVP)
- **CI/CD Ops**: For hardware lab access (RDMA NIC, GPU, multi-node cluster)

---

## Part 6: Communication & Handoff

### Weekly Stakeholder Updates
**Frequency**: Every Friday 4pm
**Audience**: Project sponsors, tech leadership, product team

**Template**:
```
## MVP Delivery Status (Week X/20)

### Completed This Week
- [ ] List of closed issues / merged PRs
- [ ] Metrics: code coverage +X%, performance baseline established

### On Track / At Risk
- [ ] Phase N: [Status]
- [ ] Risk Registry: [Any probability/impact changes]

### Next Week Focus
- [ ] Critical milestone / spike

### Blockers
- [ ] None / [List with owner and ETA]
```

### Milestone Gate Reviews
At the end of each milestone (Weeks 4, 14, 18, 20):
- [ ] All acceptance criteria verified
- [ ] Performance targets confirmed
- [ ] Stakeholder sign-off before proceeding

### Handoff to Phase 2 (Post-MVP)
**Deliverables** at end of Week 20:
1. **Source Code**: Merged to `main` branch, tagged `v1.0.0-mvp`
2. **Documentation**:
   - API reference (auto-generated doxygen)
   - Architecture guide (`.md` files in `/docs/reports`)
   - Example programs (in `/examples`)
3. **Build/Test Artifacts**:
   - CMake configuration (with `find_package(P2P)` support)
   - Docker image with all dependencies (for reproducible builds)
   - Performance baseline CSV (for regression detection in Phase 2)
4. **Known Issues & Limitations**:
   - GPU memory (deferred to Phase 2)
   - Python bindings (deferred to Phase 2)
   - Multi-path RDMA (deferred to Phase 3)
   - MemoryPool and RegistrationCache (deferred to Phase 2)

---

## Part 7: Appendix – Rationale for Key Decisions

### Why Parallelize US2 and US5?

**Connection Management (US2)** and **Memory Registration (US5)** are independent once Config exists:
- US2 uses UCX's `ucp_ep_create()` and connection establishment
- US5 uses UCX's `ucp_mem_map()` and pin/unpin
- No cross-dependencies between them

**Parallelizing saves ~5 weeks** on critical path:
- Sequential: US6 (4) + US2 (5) + US1 (5) = 14 weeks
- Parallel: US6 (4) + max(US2, US5) (5) + US1 (5) = 14 weeks
- **But**: Reduces schedule from 20 weeks to ~16 weeks because US5 starts at Week 5 instead of Week 10

**Tradeoff**: Requires 2 independent feature teams. With 6–8 person team, this is feasible.

---

### Why Defer Python Bindings to Phase 2?

**MVP is C++ only** because:
1. **Performance**: C++ is the control path for benchmarking; Python adds abstraction overhead
2. **Complexity**: nanobind + GIL management + async interop adds 3–4 weeks of work
3. **Value**: C++ customers (ML frameworks, HPC) can unblock Phase 2 features while Python team works in parallel
4. **Risk**: Python bindings can be added later without core architecture changes (plugin-like design)

**Phase 2 will include** native Python bindings with:
- `pip install p2p_transport`
- Async support (asyncio integration)
- Tensor/numpy zero-copy interop

---

### Why Focus on Tag Messaging (US1) Before RDMA (US3)?

**Tag messaging delivers immediate value**:
- Synchronous send/recv is the mental model customers understand (like MPI)
- Enables gradient synchronization, KV Cache transfer, AllGather-like patterns
- Can run on TCP fallback (no RDMA hardware required)
- Validates the entire stack end-to-end

**RDMA is performance enhancement**, not required for correctness:
- Put/get is 30–50% faster than tag_send for large messages
- But tag_send works for development/testing
- Can be added later without breaking customer code

This staging lets customers start using the library immediately while performance team optimizes RDMA path in parallel.

---

## Conclusion

The recommended 16–20 week MVP delivery plan balances:
- **Speed**: Parallelizes work where possible; delivers messaging value by Week 14
- **Risk**: Front-loads research spikes (RDMA, registration, Ascend); has fallback strategies
- **Quality**: Integrates performance benchmarking and testing throughout; meets 80% coverage gate

**Go/No-Go Decision Points**:
1. **Week 7** (after RDMA spike): Proceed to US3 or pivot to Phase 2?
2. **Week 14** (after Milestone 2): Ship messaging-only MVP or wait for RDMA?
3. **Week 18** (after Milestone 3): Ship full MVP or continue optimization phase?

With this plan, the team can deliver a **production-ready, high-performance P2P transport library** that unlocks gradient synchronization, KV Cache transfer, and parameter server use cases for both NVIDIA GPU and (Phase 2) Ascend NPU ecosystems.

---

**Document Version**: 1.0
**Last Updated**: 2026-03-04
**Next Review**: After Phase 1 completion (Week 5)
