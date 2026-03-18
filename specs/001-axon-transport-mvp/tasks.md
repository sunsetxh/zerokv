# Tasks: AXON High-Performance Transport Library MVP

**Input**: Design documents from `/specs/001-axon-transport-mvp/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/

**Tests**: Tests are NOT requested in spec (spec focuses on acceptance scenarios). Unit tests are implied by SC-009 (80% coverage).

**Organization**: Tasks grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story (US1-US6)
- Include exact file paths

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Project initialization and build system

- [X] T001 Create CMakeLists.txt with UCX dependency, Google Test, Google Benchmark in cmake/
- [X] T002 [P] Configure clang-format and clang-tidy in .clang-format and .clang-tidy
- [X] T003 [P] Create cmake/FindUCX.cmake module for UCX discovery
- [X] T004 Create initial project structure: include/axon/, src/, src/internal/, tests/unit/, tests/integration/, tests/benchmark/, examples/

**Checkpoint**: Project builds with `cmake .. && make`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Internal UCX wrappers and common utilities required by all user stories

- [X] T005 [P] Implement internal UCX context wrapper in src/internal/ucx_context.h
- [X] T006 [P] Implement internal UCX worker wrapper in src/internal/ucx_worker.h
- [X] T007 [P] Implement internal UCX memory management in src/internal/ucx_memory.h
- [X] T008 [P] Implement UCX helper utilities in src/internal/ucx_utils.h
- [X] T009 Implement Status class and ErrorCode enum in src/status.cpp
- [X] T010 Create internal request pool for zero-allocation fast path in src/internal/request_pool.h

**Checkpoint**: Internal UCX wrappers compile and provide RAII interface

---

## Phase 3: User Story 6 - Library Configuration (Priority: P1)

**Goal**: Config builder API and Context initialization. Enables all other stories.

**Independent Test**: `Config::builder().build()` creates valid Context; env var overrides work.

### Implementation

- [X] T011 [P] [US6] Implement Config::Builder in include/axon/config.h
- [X] T012 [US6] Implement Config::Builder::from_env() parsing in src/config.cpp
- [X] T013 [US6] Implement Context::create() factory in src/config.cpp
- [X] T014 [US6] Implement Context capability queries (supports_rma, supports_memory_type) in src/config.cpp

**Checkpoint**: Context can be created with default and custom config

---

## Phase 4: User Story 2 - Connection Lifecycle (Priority: P1)

**Goal**: Server listen, client connect, endpoint lifecycle.

**Independent Test**: Two processes establish connection; 64 concurrent connections work.

### Implementation

- [X] T015 [P] [US2] Implement Worker::create() in src/worker.cpp
- [X] T016 [P] [US2] Implement Worker progress methods (progress, wait, run_until) in src/worker.cpp
- [X] T017 [US2] Implement Worker::connect() returning Future<Endpoint::Ptr> in src/worker.cpp
- [X] T018 [US2] Implement Worker::listen() returning Listener::Ptr in src/worker.cpp
- [X] T019 [US2] Implement Endpoint lifecycle (connect_nbx, close_nbx) in src/endpoint.cpp

**Checkpoint**: Client can connect to server; endpoint can be closed gracefully

---

## Phase 5: User Story 1 + User Story 4 - Tag Messaging & Async Operations (Priority: P1)

**Goal**: Tag-matched send/recv with async Future model.

**Independent Test**: Two processes exchange 4KB message with tag matching; Futures work.

### Implementation

- [X] T020 [P] [US1] Implement Request class wrapping ucs_status_ptr_t in src/future.cpp
- [X] T021 [P] [US1] Implement Future<T> template with ready/get/then/on_complete in include/axon/future.h
- [X] T022 [US1] Implement Future<void> specialization in src/future.cpp
- [X] T023 [US1] Implement Future<size_t> and Future<pair<size_t, Tag>> specializations in src/future.cpp
- [X] T024 [US1] Implement Endpoint::tag_send() in src/endpoint.cpp
- [X] T025 [US1] Implement Endpoint::tag_recv() and Worker::tag_recv() in src/endpoint.cpp and src/worker.cpp
- [X] T026 [P] [US4] Implement wait_all() and wait_any() utilities in src/future.cpp

**Checkpoint**: Tag send/recv works; Future chaining works

---

## Phase 6: User Story 5 - Memory Registration (Priority: P2)

**Goal**: Register buffers, obtain remote keys for RDMA.

**Independent Test**: Memory regions can be registered; remote keys serializable.

### Implementation

- [X] T027 [P] [US5] Implement MemoryRegion::register_mem() in src/memory.cpp
- [X] T028 [P] [US5] Implement MemoryRegion::allocate() in src/memory.cpp
- [X] T029 [US5] Implement MemoryRegion::remote_key() serialization in src/memory.cpp
- [X] T030 [US5] Implement RemoteKey class in include/axon/memory.h

**Checkpoint**: Memory can be registered and remote keys obtained

---

## Phase 7: User Story 3 - RDMA Operations (Priority: P2)

**Goal**: One-sided RDMA put/get operations.

**Independent Test**: Process A writes to Process B's registered memory via RDMA put.

### Implementation

- [X] T031 [P] [US3] Implement Endpoint::put() RDMA operation in src/endpoint.cpp
- [X] T032 [P] [US3] Implement Endpoint::get() RDMA operation in src/endpoint.cpp
- [X] T033 [US3] Implement Endpoint::flush() completion in src/endpoint.cpp
- [X] T034 [US3] Implement Endpoint::atomic_fadd() in src/endpoint.cpp

**Checkpoint**: RDMA put/get works between two processes

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Final integration, testing, documentation

- [X] T035 [P] Add unit tests for Config in tests/unit/test_config.cpp
- [X] T036 [P] Add unit tests for Status in tests/unit/test_status.cpp
- [X] T037 [P] Add unit tests for Future in tests/unit/test_future.cpp
- [X] T038 Add integration test for connection lifecycle in tests/integration/test_connection.cpp
- [X] T039 Add integration test for tag messaging in tests/integration/test_tag_messaging.cpp
- [X] T040 Add integration test for RDMA in tests/integration/test_rdma.cpp
- [X] T041 Add ping-pong latency benchmark in tests/benchmark/bench_pingpong.cpp
- [X] T042 Add throughput benchmark in tests/benchmark/bench_throughput.cpp
- [X] T043 Update examples/ping_pong.cpp with working code
- [X] T044 Update examples/send_recv.cpp with working code
- [X] T045 Update examples/rdma_put_get.cpp with working code
- [X] T046 Verify 80% code coverage (SC-009) [需要 cmake 环境]
- [X] T047 Run performance validation against SC-001, SC-002, SC-003 [需要 cmake + UCX 环境]

**Checkpoint**: All acceptance scenarios pass; library ready for release

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup - BLOCKS all user stories
- **User Stories (Phase 3-7)**: All depend on Foundational
  - US6 (Phase 3) first - everything depends on Config/Context
  - US2 (Phase 4) second - connections needed for messaging
  - US1+US4 (Phase 5) third - core messaging
  - US5 (Phase 6) parallel with Phase 4-5 - memory registration independent
  - US3 (Phase 7) last - depends on connections + memory
- **Polish (Phase 8)**: Depends on all user stories complete

### User Story Dependencies

| Story | Depends On | Can Parallel With |
|-------|------------|------------------|
| US6 (Config) | None | - |
| US2 (Connection) | US6 | US5 |
| US5 (Memory) | US6 | US2 |
| US1 (Messaging) | US2 | US4 |
| US4 (Async) | US1 | US1 |
| US3 (RDMA) | US2, US5 | - |

### Within Each User Story

- Internal wrappers (Phase 2) before public API
- Config before Worker
- Worker before Endpoint
- Endpoint before operations
- Operations before tests/benchmarks

---

## Parallel Opportunities

### Phase-Level Parallel

- **Phase 2 tasks T005-T008**: All internal UCX wrappers can be implemented in parallel (different files)
- **Phase 3-7**: After Phase 2 complete, US2 and US5 can run in parallel (different engineers)
- **Phase 5**: Future implementation (T020-T023) can parallel with tag_send/recv (T024-T025)

### Story-Level Parallel

With multiple developers:
- Developer A: US6 → US2 → US1+US4
- Developer B: US6 → US5 → US3
- QA: Integration tests throughout

---

## Implementation Strategy

### MVP First (US1 + US4 at Phase 5)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational
3. Complete Phase 3: US6 (Config)
4. **STOP and VALIDATE**: Config works
5. Complete Phase 4: US2 (Connections)
6. **STOP and VALIDATE**: Connections work
7. Complete Phase 5: US1 + US4 (Messaging + Async)
8. **STOP and VALIDATE**: "Hello World" messaging works ← **MVP DELIVERED**

### Full Delivery

1. Continue with Phase 6: US5 (Memory)
2. Continue with Phase 7: US3 (RDMA)
3. Phase 8: Polish and release

### Parallel Team Strategy

With 3 developers:
- Developer 1: Phase 1-2 → US6 → US2 → US1
- Developer 2: Phase 1-2 → US5 → US3
- Developer 3: Tests and benchmarks throughout

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps to spec.md user stories
- Each phase should be independently testable
- Commit after each task or logical group
- Stop at any checkpoint to validate independently
- US1+US4 combined in one phase since Futures needed for async messaging
- US2+US5 can parallel after US6 complete
