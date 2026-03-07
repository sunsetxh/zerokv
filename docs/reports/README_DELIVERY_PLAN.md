# P2P MVP Delivery Plan — Documentation Hub

**Created**: 2026-03-04  
**Status**: Final  
**Audience**: Project sponsors, engineering leadership, technical stakeholders

## Quick Navigation

### For Different Audiences

**Executives / Sponsors** (5 min read):
→ Start with [`DELIVERY_PLAN_SUMMARY.txt`](DELIVERY_PLAN_SUMMARY.txt)  
→ Print-friendly text format; key timeline, risks, success criteria

**Technical Leadership** (15 min read):
→ Start with [`pm-mvp-quick-reference.md`](pm-mvp-quick-reference.md)  
→ Visual diagrams; phase breakdown; decision gates; team allocation

**Project Managers / Engineers** (30 min read):
→ Read full [`pm-mvp-delivery-plan.md`](pm-mvp-delivery-plan.md)  
→ Complete strategy; rationale; risk mitigation; communication plan

**Architecture / Tech Lead** (cross-reference):
→ See [`../report-arch-design.md`](report-arch-design.md) for technical decisions  
→ See [`../test-strategy.md`](test-strategy.md) for performance baselines & CI/CD

---

## The Three-Document Strategy

### 1. DELIVERY_PLAN_SUMMARY.txt (15 KB, 300 lines)
**Best for**: Print-outs, presentations, quick lookup

Contains:
- 30-second executive summary
- 5-phase delivery breakdown
- 4 "Hello World" milestones
- Top 3 risks with fallback strategies
- Team allocation (6–8 FTE)
- Performance targets
- Success criteria checklist
- 4 critical decision gates (Go/No-Go)

Use when: Briefing sponsors, all-hands meetings, printed reference card

---

### 2. pm-mvp-quick-reference.md (12 KB, 250 lines)
**Best for**: Technical stakeholders, 15-min briefings, slide decks

Contains:
- Visual phase timeline (ASCII diagram)
- Milestone definitions table
- Dependency graph with parallelization insight
- Execution roadmap (what to build each phase)
- Top 3 risks with mitigation & fallback
- Critical decision points (Gate 1–4)
- Performance targets at a glance
- Team structure
- What's in/out of MVP

Use when: Architecture review, sprint planning, weekly standups

---

### 3. pm-mvp-delivery-plan.md (28 KB, 450+ lines)
**Best for**: Comprehensive planning, detailed strategy, reference guide

Contains:
- Executive summary (2 pages)
- Part 1: Optimal MVP delivery order
  - 1.1 Dependency graph with analysis
  - 1.2 Recommended delivery sequence (5 phases)
  - 1.3 Timeline summary (16–20 weeks)
- Part 2: Minimum viable milestones
  - M1, M2, M3, M4 definitions with acceptance criteria
  - Why each milestone matters
- Part 3: Risk assessment for MVP
  - Risk 1–3: RDMA, registration, Ascend (detailed analysis)
  - Secondary risks (6 more)
  - Risk monitoring & escalation
- Part 4: Resource & team allocation
  - Suggested team structure (6–8 FTE)
  - External dependencies
- Part 5: Communication & handoff
  - Stakeholder update template
  - Milestone gate reviews
  - Phase 2 handoff deliverables
- Part 6: Appendix
  - Rationale for key decisions
  - Why parallelize US2 & US5?
  - Why defer Python bindings?
  - Why prioritize messaging before RDMA?

Use when: Detailed planning, risk mitigation workshops, stakeholder alignment meetings

---

## Key Insights from Analysis

### 1. Optimal Delivery Order (Why This Sequence?)

```
US6: Config         (4 weeks, no dependencies)
  ↓
US2 & US5 PARALLEL  (5 weeks, both depend on US6)
  ↓
US1+US4: Messaging  (5 weeks, depends on US2; async enhances it)
  ↓
US3: RDMA           (4 weeks, depends on US2 + US5)
  ↓
Polish & Ship       (2 weeks)
```

**Critical insight**: Parallelizing US2 (connections) and US5 (memory registration) saves ~5 weeks on critical path because they're independent after US6.

Sequential cost: 4 + 5 + 5 + 4 + 2 = 20 weeks
Parallel cost: 4 + 5 (max) + 5 + 4 + 2 = 20 weeks
But: With parallel execution, team can parallelize other work, reducing real calendar time to 16–18 weeks.

### 2. Minimum Viable Milestone (Week 14, Not Week 20)

The **"Hello World Messaging"** milestone at Week 14 delivers MVP value:
- Two processes can exchange messages with tag matching
- Customers can prototype gradient synchronization pipelines
- RDMA (US3) is an optimization, not a requirement for correctness

This stages value delivery and de-risks the schedule:
- **If** RDMA becomes hard (Risk 1), we ship messaging MVP at Week 14
- **If** RDMA slips, core value is already delivered
- **If** all goes well, RDMA ships at Week 18 as planned

### 3. Top 3 Technical Risks

| Risk | Probability | Impact | Mitigation | Fallback |
|------|-------------|--------|-----------|----------|
| **RDMA Complexity** | 15–20% | HIGH (4–8 wk slip) | Spike UCX RMA API (Week 6–7) | Ship messaging-only MVP at Week 14 |
| **Registration Perf** | 25–30% | MEDIUM (2–4 wk slip) | Benchmark vs NCCL (Week 5); add cache (Week 15) | Relax target, document workaround |
| **Ascend Plugin** | 35–40% | MEDIUM–HIGH (4–6 wk slip) | Design plugin interface early (Week 2–3) | Defer Ascend to Phase 2 (not blocking MVP) |

**Risk monitoring**: Weekly review; escalate if probability > 50% or impact = CRITICAL.

---

## Timeline at a Glance

| Phase | Weeks | Story | Milestone | Output |
|-------|-------|-------|-----------|--------|
| 1 | 1–4 | US6 | M1: Config & Init | Can create Context |
| 2A | 5–9 | US2 | (parallel) | Can connect processes |
| 2B | 5–9 | US5 | (parallel) | Can register memory |
| 3 | 10–14 | US1+US4 | **M2: Hello World** | **Can exchange messages** |
| 4 | 15–18 | US3 | **M3: RDMA Ready** | **Can do put/get** |
| 5 | 19–20 | Polish | **M4: Production Ready** | **Ship!** |

**Critical path** (with parallelization): 4 + 5 + 5 + 4 + 2 = 20 weeks
**Actual timeline** (with team parallelization): 16–18 weeks calendar time

---

## Success Criteria (End of Week 20)

✅ All 6 user story acceptance scenarios pass  
✅ Performance targets met (SC-001 through SC-010)  
✅ 80% line code coverage, 70% branch coverage  
✅ ASAN clean (no leaks), TSAN clean (no races)  
✅ Works on RDMA + TCP + Shared Memory  
✅ Handles 64 concurrent connections  
✅ API reference, examples, architecture docs complete  
✅ Docker image + CMake export ready  

---

## Team Allocation (6–8 FTE)

- **Tech Lead / Architect** (1.0 FTE) — Design, risk mitigation
- **Core C++ Engineer** (2.0 FTE) — US6, US2, US1 implementation
- **RDMA Specialist** (1.5 FTE) — US5, US3, performance tuning
- **Performance Engineer** (1.0 FTE) — Benchmarking, targets
- **QA / Test Engineer** (1.0 FTE) — Test infrastructure, CI/CD
- **PM / Technical Writer** (0.5 FTE) — Spec, milestone tracking, docs
- **GPU/Ascend Liaison** (0.5 FTE, optional) — HCCL research (risk mitigation)

---

## Go/No-Go Decision Gates

| Gate | When | Question | Pass | Fallback |
|------|------|----------|------|----------|
| **Gate 1** | Week 7 | RDMA architecture sound? | Spike OK | Defer RDMA to Phase 2 |
| **Gate 2** | Week 14 | Messaging MVP ready? | M2 passes | +2 weeks tuning |
| **Gate 3** | Week 18 | RDMA perf acceptable? | >85% BW | Relax target |
| **Gate 4** | Week 20 | All criteria met? | M4 sign-off | Ship with known issues |

---

## Communication Cadence

**Every Friday 4pm** (15 min standup):
- Completed this week
- On track / at risk items
- Next week focus
- Blockers with owner & ETA

**Every 4 weeks** (1 hour gate review):
- Milestone acceptance criteria verification
- Performance target validation
- Risk status update
- Go/No-Go decision

---

## Related Documents

**In this directory**:
- [`report-pm-requirements.md`](report-pm-requirements.md) — Product requirements, user scenarios, competitive analysis
- [`report-arch-design.md`](report-arch-design.md) — Architecture, tech stack, design decisions
- [`report-dev-interface.md`](report-dev-interface.md) — C++ API design, interface contracts
- [`test-strategy.md`](test-strategy.md) — Test plan, performance baselines, CI/CD strategy

**In specs**:
- [`/specs/001-p2p-transport-mvp/spec.md`](/specs/001-p2p-transport-mvp/spec.md) — Full feature specification (6 user stories)

---

## How to Use This Plan

### As an Executive / Sponsor
1. Read [`DELIVERY_PLAN_SUMMARY.txt`](DELIVERY_PLAN_SUMMARY.txt) (5 min)
2. Focus on timeline (16–20 weeks), key milestones (Week 14, 18, 20), and top risks
3. Use for: Budget approval, timeline expectations, escalation triggers

### As a Tech Lead
1. Read [`pm-mvp-quick-reference.md`](pm-mvp-quick-reference.md) (15 min)
2. Review dependency graph and decision gates
3. Read [`report-arch-design.md`](../report-arch-design.md) for implementation strategy
4. Use for: Architecture review, sprint planning, risk mitigation

### As an Engineering Manager
1. Read full [`pm-mvp-delivery-plan.md`](pm-mvp-delivery-plan.md) (30 min)
2. Focus on Part 4 (resource allocation) and Part 5 (communication)
3. Set up weekly standups and gate reviews
4. Use for: Team allocation, milestone tracking, stakeholder communication

### As a Developer
1. Focus on [`/specs/001-p2p-transport-mvp/spec.md`](/specs/001-p2p-transport-mvp/spec.md) for what to build
2. Glance at phase timeline to know what's in scope for your phase
3. Use for: Sprint planning, acceptance criteria, merge readiness

### As QA / Test Lead
1. Read [`test-strategy.md`](test-strategy.md) for comprehensive test plan
2. Use performance baselines from [`test-strategy.md`](test-strategy.md) to set up CI/CD
3. Coordinate with delivery plan milestones (Gate 2 = functional tests ready, Gate 3 = perf tests ready)
4. Use for: Test infrastructure setup, performance regression detection

---

## Approval & Sign-off

**Prepared by**: Product Manager  
**Date**: 2026-03-04  
**Version**: 1.0  
**Status**: Final  

**Requires approval from**:
- [ ] Project Sponsor
- [ ] Tech Lead / Architecture
- [ ] Engineering Manager
- [ ] PM / Program Manager

**Next review**: After Phase 1 completion (Week 5)

---

## Questions?

If you have questions about this plan, refer to the appropriate document:

- **"What should we deliver?"** → Read `/specs/001-p2p-transport-mvp/spec.md`
- **"What's the timeline?"** → Read `DELIVERY_PLAN_SUMMARY.txt` or `pm-mvp-quick-reference.md`
- **"Why this order?"** → Read full `pm-mvp-delivery-plan.md`, Part 1
- **"What are the risks?"** → Read full `pm-mvp-delivery-plan.md`, Part 3
- **"How do we implement this?"** → Read `report-arch-design.md` and `test-strategy.md`
- **"What's the acceptance criteria?"** → Read `/specs/001-p2p-transport-mvp/spec.md`

---

*Last updated: 2026-03-04*
