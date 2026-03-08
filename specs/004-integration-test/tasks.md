# Tasks: ZeroKV 集成测试方案

**Input**: Design documents from `/specs/004-integration-test/`
**Prerequisites**: plan.md (required), spec.md (required for user stories), quickstart.md

**Tests**: Test tasks are included as the feature is specifically about testing infrastructure.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Project initialization and test infrastructure setup

- [X] T001 [P] Verify ZeroKV build is successful with tests enabled in build/
- [X] T002 Create test fixtures for server lifecycle management in tests/integration/fixtures.py
- [X] T003 [P] Create pytest configuration in tests/integration/conftest.py

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core test infrastructure that MUST be complete before ANY user story can be implemented

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T004 Implement TestServer class wrapper in tests/integration/fixtures.py
- [X] T005 Implement TestClient base class for Python in tests/integration/client_wrapper.py
- [X] T006 [P] Create C++ test fixtures in tests/integration/test_server_fixture.h
- [X] T007 Setup test server startup/shutdown lifecycle in conftest.py

**Checkpoint**: Foundation ready - user story implementation can now begin in parallel

---

## Phase 3: User Story 1 - 服务端与客户端集成测试 (Priority: P1) 🎯 MVP

**Goal**: Verify basic put/get/delete operations work end-to-end between server and client

**Independent Test**: Start server, connect client, perform put/get operations, verify data integrity

### Tests for User Story 1

- [X] T008 [P] [US1] Python integration test for put/get operations in python/tests/test_zerokv.py
- [X] T009 [P] [US1] Python integration test for delete operations in python/tests/test_zerokv.py
- [X] T010 [US1] C++ integration test for basic operations in tests/integration/test_cluster.cc

### Implementation for User Story 1

- [X] T011 [US1] Run all US1 tests and verify they pass
- [X] T012 [US1] Document test results in quickstart.md

**Checkpoint**: At this point, User Story 1 should be fully functional and testable independently

---

## Phase 4: User Story 2 - 批量操作集成测试 (Priority: P1)

**Goal**: Verify batch_put and batch_get operations work correctly between server and client

**Independent Test**: Client performs batch_put, then batch_get, verifies all values match

### Tests for User Story 2

- [X] T013 [P] [US2] Python integration test for batch operations in python/tests/test_zerokv.py
- [X] T014 [US2] C++ integration test for batch operations in tests/integration/test_batch.cc

### Implementation for User Story 2

- [X] T015 [US2] Run all US2 tests and verify they pass

**Checkpoint**: At this point, User Stories 1 AND 2 should both work independently

---

## Phase 5: User Story 3 - 故障恢复测试 (Priority: P2)

**Goal**: Verify system handles network interruptions and server restarts gracefully

**Independent Test**: Simulate server crash, verify reconnection works after recovery

### Tests for User Story 3

- [X] T016 [P] [US3] Python test for server restart scenario in python/tests/test_zerokv.py
- [X] T017 [US3] Python test for network timeout handling in python/tests/test_zerokv.py

### Implementation for User Story 3

- [X] T018 [US3] Run all US3 tests and verify they pass

---

## Phase 6: User Story 4 - 性能基准测试 (Priority: P2)

**Goal**: Measure system performance under load with latency and throughput metrics

**Independent Test**: Run benchmark with specified concurrent connections and operations, measure latency/throughput

### Tests for User Story 4

- [X] T019 [P] [US4] Python benchmark for single client latency in python/benchmark/benchmark.py
- [X] T020 [P] [US4] Python benchmark for concurrent clients in python/benchmark/benchmark.py

### Implementation for User Story 4

- [X] T021 [US4] Run benchmarks and document baseline performance in docs/PERFORMANCE.md
- [X] T022 [US4] Verify performance meets success criteria (latency < 10ms for 1000 ops)

---

## Phase 7: User Story 5 - 多语言API测试 (Priority: P3)

**Goal**: Verify Python and C++ clients can interoperate (data written by one language readable by other)

**Independent Test**: C++ client puts value, Python client reads same value, verify data matches

### Tests for User Story 5

- [X] T023 [P] [US5] Python test for cross-language read after C++ write in python/tests/test_zerokv.py
- [X] T024 [P] [US5] C++ test for cross-language read after Python write in tests/integration/test_cluster.cc

### Implementation for User Story 5

- [X] T025 [US5] Run all US5 tests and verify they pass

**Checkpoint**: All user stories should now be independently functional

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Improvements that affect multiple user stories

- [X] T026 [P] Update quickstart.md with test execution instructions
- [X] T027 Create GitHub Actions workflow for CI integration in .github/workflows/integration-tests.yml
- [X] T028 Verify all tests pass in CI environment
- [X] T029 Add test coverage reporting

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phase 3+)**: All depend on Foundational phase completion
  - User stories can then proceed in parallel (if staffed)
  - Or sequentially in priority order (P1 → P2 → P3)
- **Polish (Final Phase)**: Depends on all desired user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational (Phase 2) - No dependencies on other stories
- **User Story 2 (P1)**: Can start after Foundational (Phase 2) - May integrate with US1 but should be independently testable
- **User Story 3 (P2)**: Can start after Foundational (Phase 2) - May integrate with US1/US2 but should be independently testable
- **User Story 4 (P2)**: Can start after Foundational (Phase 2) - Independent of other stories
- **User Story 5 (P3)**: Can start after Foundational (Phase 2) - Depends on US1 and US2 for basic operations

### Within Each User Story

- Tests MUST be written and FAIL before implementation
- Tests run first, implementation verified by test pass
- Story complete before moving to next priority

### Parallel Opportunities

- All Setup tasks marked [P] can run in parallel
- All Foundational tasks marked [P] can run in parallel (within Phase 2)
- Tests for each user story marked [P] can run in parallel
- Once Foundational phase completes, all user stories can start in parallel (if team capacity allows)

---

## Parallel Example: User Story 1

```bash
# Launch all tests for User Story 1 together:
Task: "Python integration test for put/get operations in python/tests/test_zerokv.py"
Task: "Python integration test for delete operations in python/tests/test_zerokv.py"
Task: "C++ integration test for basic operations in tests/integration/test_cluster.cc"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational (CRITICAL - blocks all stories)
3. Complete Phase 3: User Story 1
4. **STOP and VALIDATE**: Test User Story 1 independently
5. Deploy/demo if ready

### Incremental Delivery

1. Complete Setup + Foundational → Foundation ready
2. Add User Story 1 → Test independently → Deploy/Demo (MVP!)
3. Add User Story 2 → Test independently → Deploy/Demo
4. Add User Story 3 → Test independently → Deploy/Demo
5. Add User Story 4 → Test independently → Deploy/Demo
6. Add User Story 5 → Test independently → Deploy/Demo
7. Each story adds value without breaking previous stories

### Parallel Team Strategy

With multiple developers:

1. Team completes Setup + Foundational together
2. Once Foundational is done:
   - Developer A: User Story 1 & 2 (both P1)
   - Developer B: User Story 3 (P2)
   - Developer C: User Story 4 & 5 (P2/P3)
3. Stories complete and integrate independently

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story should be independently completable and testable
- Verify tests fail before implementing
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- Avoid: vague tasks, same file conflicts, cross-story dependencies that break independence

---

## Task Summary

| Phase | Description | Tasks |
|-------|-------------|-------|
| Phase 1 | Setup | T001-T003 |
| Phase 2 | Foundational | T004-T007 |
| Phase 3 | US1: Server-client integration (P1) | T008-T012 |
| Phase 4 | US2: Batch operations (P1) | T013-T015 |
| Phase 5 | US3: Failure recovery (P2) | T016-T018 |
| Phase 6 | US4: Performance benchmarks (P2) | T019-T022 |
| Phase 7 | US5: Multi-language API (P3) | T023-T025 |
| Phase 8 | Polish & CI/CD | T026-T029 |

**Total Tasks**: 29
**MVP Scope**: Phase 1-3 (User Story 1) - Tasks T001-T012
