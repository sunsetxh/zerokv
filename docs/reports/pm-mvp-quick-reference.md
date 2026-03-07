# P2P MVP Delivery Plan — Quick Reference

**For**: Project sponsors, engineering leadership, technical stakeholders
**Reading Time**: 5 minutes
**Full Details**: See `pm-mvp-delivery-plan.md`

---

## The 5-Phase Delivery Strategy

```
┌─────────────────────────────────────────────────────────────────┐
│ P2P Transport Library MVP – 16–20 Week Delivery Plan            │
│ Team: 6–8 FTE | Critical Path: 13 weeks (US6→US2→US1)         │
└─────────────────────────────────────────────────────────────────┘

Phase 1: Foundation        Phase 2: Parallel Growth    Phase 3: Core Value  Phase 4: Advanced  Phase 5: Polish
Weeks 1–4                  Weeks 5–9                    Weeks 10–14          Weeks 15–18       Weeks 19–20
┌──────────────┐          ┌──────────┬──────────┐      ┌──────────┐         ┌────────┐         ┌──────────┐
│ US6: Config  │────────▶ │ US2:     │ US5:     │ ────▶│ US1+US4: │────────▶│ US3:   │────────▶│ Perf &   │
│ (Setup)      │          │ Connect  │ Memory   │      │ Messaging│         │ RDMA   │         │ Polish   │
└──────────────┘          │ (Conn)   │ (Reg)    │      │ (Async)  │         │ (Put/  │         │ (Ready)  │
                          └──────────┴──────────┘      │          │         │  Get)  │         │          │
                          (parallel = save 5 weeks)    │ ✓ Value! │         │ ✓Done! │         │ ✓ Ship!  │
                                                        └──────────┘         └────────┘         └──────────┘
```

---

## Four "Hello World" Milestones

| Milestone | When | What | Why | Value Unlock |
|-----------|------|------|-----|--------------|
| **M1: Config & Init** | Week 4 | Create Context with config builder | Validate build system, dependencies | Foundation for all features |
| **M2: Hello World Messaging** | Week 14 | Send/recv 4KB with tag matching | Core value prop; customers can build pipelines | Gradient sync, KV Cache transfer |
| **M3: RDMA Ready** | Week 18 | Put/get with remote keys | Complete P2P suite | Parameter servers, low-latency inference |
| **M4: Production Ready** | Week 20 | All acceptance criteria, perf targets met, 80% coverage | Ship-ready | Ready for customer deployment |

---

## Dependency Graph (Why This Order?)

```
                US6: Config (Week 1)
                    ↓
         ┌──────────┴──────────┐
         ↓                     ↓
    US2: Connect          US5: Memory Reg.
    (Weeks 5–9)           (Weeks 5–9)
         │                     │
         └──────────┬──────────┘
                    ↓
         US1: Tag Messaging (Weeks 10–14)
              ✓ VALUE UNLOCK ✓
                    ↓
         US4: Async Model (same as US1)
              (Future/callbacks)
                    ↓
         US3: RDMA Put/Get (Weeks 15–18)
              (uses US2 + US5)
```

**Key Insight**: US2 and US5 can run in parallel (save 5 weeks). Everything else is sequential on critical path.

---

## Execution Roadmap

### Phase 1: Foundation (Weeks 1–4)
**What**: Build system, Config API, Context initialization
**Acceptance**: Can create Context; config validation works
**Team**: 1 Core C++ Eng + Tech Lead
```
Config().set_worker_count(4).build()  ✓ Works
```

### Phase 2A: Connections (Weeks 5–9, in parallel with 2B)
**What**: Server listen, client connect, endpoint lifecycle
**Acceptance**: Two processes establish connection
**Team**: 1 Core C++ Eng
```
server.listen("tcp://0.0.0.0:1234")
client.connect("tcp://localhost:1234")  ✓ Works
```

### Phase 2B: Memory Registration (Weeks 5–9, in parallel with 2A)
**What**: Register buffers, pin pages, get remote keys
**Acceptance**: Memory region registered and serialized
**Team**: 1 RDMA Specialist
```
auto mr = ctx->register_memory(buf, 1MB);
auto rkey = mr->remote_key();  ✓ Works
```

### Phase 3: Tag Messaging + Async (Weeks 10–14)
**What**: Synchronous tag_send/recv, async Futures
**Acceptance**: Two processes exchange messages; Futures work
**Milestone**: ✓ **M2: Hello World Messaging**
**Team**: 1–2 Core C++ Eng + Performance Eng
```
ep->tag_send(buf, 1024, 42);         // blocking
ep->tag_recv(buf, 1024, 42);         // blocking
auto fut = ep->tag_send_nb(...);     // async
fut->wait();  ✓ Works
```

### Phase 4: RDMA Put/Get (Weeks 15–18)
**What**: One-sided remote memory access
**Acceptance**: Write/read to remote memory with zero local CPU overhead
**Milestone**: ✓ **M3: RDMA Ready**
**Team**: 1 RDMA Specialist + Performance Eng
```
ep->put(local, 1GB, remote_addr, remote_key);
ep->flush();  ✓ Works (>90% line bandwidth)
```

### Phase 5: Performance & Polish (Weeks 19–20)
**What**: Tuning, testing, documentation, ship
**Acceptance**: All success criteria met; 80% coverage; no leaks
**Milestone**: ✓ **M4: Production Ready**
**Team**: All hands for final push
```
Perf targets met (SC-001 through SC-010)
80% code coverage achieved
All tests passing
Documentation complete  ✓ SHIP!
```

---

## Top 3 Risks & Fallback Plans

### Risk 1: RDMA Complexity (Probability: 15–20%, Impact: HIGH)
**What Could Go Wrong**: UCX RMA API incomplete; RDMA put/get harder than expected

**Mitigation** (Week 6–7):
- Spike research on UCX RMA with small test program
- Identify incompatibilities early
- Decision gate at Week 8: proceed or pivot

**Fallback**: Ship messaging-only MVP (Week 14) first; RDMA → Phase 2

---

### Risk 2: Memory Registration Performance (Probability: 25–30%, Impact: MEDIUM)
**What Could Go Wrong**: Registration becomes bottleneck; can't achieve 90% bandwidth

**Mitigation** (Weeks 5–15):
- Benchmark registration speed vs NCCL (Week 5)
- Add MemoryPool (Week 12)
- Add RegistrationCache (Week 15)

**Fallback**: Relax perf target for unregistered buffers; document workaround

---

### Risk 3: Ascend NPU Plugin (Probability: 35–40%, Impact: MEDIUM–HIGH)
**What Could Go Wrong**: HCCL API incompatible with plugin design; Ascend support breaks

**Mitigation** (Weeks 2–15):
- Design plugin interface early (Week 2–3)
- Engage Ascend team (Week 6)
- Implement mock HCCL plugin (Week 12–14)
- Decision gate at Week 15

**Fallback**: Ship MVP as NVIDIA-only; Ascend support → Phase 2 (separate project)

---

## Critical Decision Points (Go/No-Go Gates)

| Gate | When | Question | Pass Criteria | Fallback |
|------|------|----------|---------------|----------|
| **Gate 1** | Week 7 | RDMA architecture sound? | Spike completes; no blockers | Defer RDMA to Phase 2 |
| **Gate 2** | Week 14 | Messaging MVP ready? | M2 acceptance tests pass | Extend Week 14 by +2 |
| **Gate 3** | Week 18 | RDMA performance acceptable? | >85% line BW achieved | Relax target; document |
| **Gate 4** | Week 20 | All acceptance criteria met? | M4 sign-off | Ship with known issues doc |

---

## Performance Targets (from Success Criteria)

| Metric | Target | Notes |
|--------|--------|-------|
| 4KB latency | < 5 us (1-way, RDMA) | Within 2us of raw UCX |
| 1GB throughput | > 92% of link BW | 200Gbps link → >23 GB/s |
| Overhead vs UCX | < 5% (for 64KB+ msgs) | Library wrapper cost |
| Connection setup | < 100ms | First connection one-time cost |
| Max concurrent conns | 64 without degradation | Per Worker |
| Code coverage | ≥ 80% lines, 70% branches | Mandatory gate |
| No memory leaks | ASAN clean | Verified in CI |

---

## Team Allocation

| Role | FTE | Key Responsibilities |
|------|-----|----------------------|
| Tech Lead / Architect | 1.0 | Design; risk mitigation; Phase 1–2 |
| Core C++ Engineer | 2.0 | US6, US2, US1 implementation |
| RDMA Specialist | 1.5 | US5, US3, performance tuning |
| Performance Engineer | 1.0 | Benchmarking; targets; Phase 5 |
| QA / Test Engineer | 1.0 | Test infrastructure; CI; fault injection |
| PM / Tech Writer | 0.5 | Spec; milestone tracking; docs |
| GPU/Ascend Liaison | 0.5 | HCCL compatibility research (optional) |
| **Total** | **7–8 FTE** | |

---

## What's NOT in MVP (Deferred to Phase 2+)

| Feature | Why Deferred | Target Phase |
|---------|-------------|--------------|
| Python bindings | Adds 3–4 weeks; can parallelize | Phase 2 |
| MemoryPool (slab allocator) | Nice-to-have for optimization | Phase 2 |
| RegistrationCache (LRU) | Nice-to-have; fallback is no cache | Phase 2 |
| Scatter/gather non-contiguous | Advanced feature; 1KB–1GB covers most cases | Phase 2 |
| Multi-path RDMA aggregation | Advanced; requires topology discovery | Phase 3 |
| NCCL/HCCL plugins | Requires external API stability | Phase 2 |
| Observability (metrics, tracing) | Production nice-to-have | Phase 3 |
| Stream operations | Alternative to tag messaging; lower priority | Phase 2 |
| Atomic operations | Niche use case | Phase 3 |

---

## Success Definition

### At End of Week 20, MVP is Done When:

- ✅ All 6 user story acceptance scenarios pass
- ✅ Performance targets met (SC-001 through SC-010)
- ✅ 80% code coverage achieved
- ✅ ASAN/TSAN clean (no leaks, no races)
- ✅ Works on RDMA + TCP + Shared Memory
- ✅ Handles 64 concurrent connections
- ✅ Fault injection tests pass (peer crash, network failure, OOM)
- ✅ Documentation and examples complete
- ✅ Docker image and CMake export ready for customers

---

## Weekly Rhythm

**Every Friday 4pm**: 15-min stakeholder update
- Completed this week (PRs, features)
- On track / at risk items
- Next week focus
- Blockers (with owner, ETA)

**Every 4 weeks (Gates)**: 1-hour review
- Milestone sign-off
- Performance verification
- Risk status update
- Go/No-Go decision

---

## Quick Links

| Document | Purpose |
|----------|---------|
| `pm-mvp-delivery-plan.md` | Full plan with rationale (this document) |
| `/specs/001-p2p-transport-mvp/spec.md` | User stories & acceptance criteria |
| `/docs/reports/report-arch-design.md` | Architecture & tech decisions |
| `/docs/reports/test-strategy.md` | Testing approach & performance baselines |

---

**Prepared by**: Product Manager
**Date**: 2026-03-04
**Version**: 1.0
**Next Review**: After Phase 1 (Week 5)

---

## TL;DR: The Elevator Pitch

> Build a production-ready P2P transport library in **20 weeks** with a **6–8 person team**:
>
> **Week 4**: Config & initialization (foundation)
> **Week 9**: Connections + memory registration (parallel)
> **Week 14**: Tag messaging (MVP with customer value)
> **Week 18**: RDMA operations (complete feature set)
> **Week 20**: Performance tuned, tested, documented (ship!)
>
> **Top risk**: RDMA complexity. **Mitigation**: Early spike + fallback to messaging-only. **Value delivery**: Week 14 (not 20).
