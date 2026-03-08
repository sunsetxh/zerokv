# Tasks: ZeroKV 客户端-服务端通信协议实现

**Input**: Design documents from `/specs/005-impl-comm-protocol/`
**Prerequisites**: plan.md (required), spec.md (required for user stories), data-model.md, contracts/api.md

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Project initialization and protocol infrastructure setup

- [X] T001 [P] Create protocol directory structure in src/protocol/
- [X] T002 [P] Define message enums in src/protocol/message.h (op_code, status, flags)
- [X] T003 Create message structure definitions in src/protocol/message.h

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core protocol implementation that MUST be complete before ANY user story can be implemented

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T004 Implement message codec (encode/decode) in src/protocol/codec.cc
- [X] T005 [P] Create protocol handler in src/protocol/handler.h
- [X] T006 Update UCXTransport to support message send/receive in src/transport/ucx_transport.cc

**Checkpoint**: Foundation ready - user story implementation can now begin in parallel

---

## Phase 3: User Story 1 - 客户端能连接到服务端并执行基本操作 (Priority: P1) 🎯 MVP

**Goal**: Client can connect to server and perform put/get/delete operations

**Independent Test**: Start server, connect client, put a key-value, get the value back, verify it matches

### Tests for User Story 1

- [ ] T007 [P] [US1] Integration test for basic put/get in python/tests/test_zerokv.py

### Implementation for User Story 1

- [X] T008 [US1] Update server to accept connections and process requests in src/server/server.cc
- [X] T009 [US1] Implement request handler for PUT operation in src/protocol/handler.cc
- [X] T010 [US1] Implement request handler for GET operation in src/protocol/handler.cc
- [X] T011 [US1] Implement request handler for DELETE operation in src/protocol/handler.cc
- [X] T012 [US1] Update client to send requests and receive responses in src/client/client.cc
- [X] T013 [US1] Run integration test and verify put/get/delete works end-to-end

**Checkpoint**: At this point, User Story 1 should be fully functional and testable independently

---

## Phase 4: User Story 2 - 批量操作支持 (Priority: P1)

**Goal**: Support batch_put and batch_get operations

**Independent Test**: Client performs batch_put with 100 key-value pairs, then batch_get, verifies all values match

### Tests for User Story 2

- [ ] T014 [P] [US2] Integration test for batch operations in python/tests/test_zerokv.py

### Implementation for User Story 2

- [ ] T015 [US2] Implement request handler for BATCH_PUT operation in src/protocol/handler.cc
- [ ] T016 [US2] Implement request handler for BATCH_GET operation in src/protocol/handler.cc
- [ ] T017 [US2] Update client batch methods to use protocol in src/client/client.cc
- [ ] T018 [US2] Run batch integration tests and verify performance

**Checkpoint**: At this point, User Stories 1 AND 2 should both work independently

---

## Phase 5: User Story 3 - 连接管理和错误处理 (Priority: P2)

**Goal**: Handle connection failures gracefully

**Independent Test**: Connect client, disconnect network, reconnect, verify operations resume

### Tests for User Story 3

- [ ] T019 [P] [US3] Test for connection retry in python/tests/test_zerokv.py

### Implementation for User Story 3

- [ ] T020 [US3] Implement connection retry logic in src/client/client.cc
- [ ] T021 [US3] Add timeout support for operations in src/protocol/codec.cc
- [ ] T022 [US3] Update error handling for network failures

---

## Phase 6: User Story 4 - 数据一致性和错误响应 (Priority: P2)

**Goal**: Return clear error messages for failed operations

**Independent Test**: Attempt invalid operations, verify appropriate error messages are returned

### Tests for User Story 4

- [ ] T023 [P] [US4] Test for not found and error responses in python/tests/test_zerokv.py

### Implementation for User Story 4

- [ ] T024 [US4] Implement NOT_FOUND status handling in src/protocol/handler.cc
- [ ] T025 [US4] Implement ERROR status handling in src/protocol/handler.cc
- [ ] T026 [US4] Update Python bindings to raise appropriate exceptions

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Improvements that affect multiple user stories

- [ ] T027 [P] Update quickstart.md with protocol testing instructions
- [ ] T028 Add performance benchmarks for single operations and batch operations
- [ ] T029 Verify all integration tests pass

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
- **User Story 2 (P1)**: Can start after Foundational (Phase 2) - Uses US1 infrastructure
- **User Story 3 (P2)**: Can start after Foundational (Phase 2) - Independent
- **User Story 4 (P2)**: Can start after Foundational (Phase 2) - Uses US1/US2 infrastructure

### Within Each User Story

- Tests MUST be written and FAIL before implementation
- Implementation verified by test pass
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
Task: "Integration test for basic put/get in python/tests/test_zerokv.py"

# Implementation can proceed:
Task: "Update server to accept connections and process requests"
Task: "Implement request handler for PUT operation"
Task: "Implement request handler for GET operation"
Task: "Implement request handler for DELETE operation"
Task: "Update client to send requests and receive responses"
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

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story should be independently completable and testable
- Verify tests fail before implementing
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently

---

## Task Summary

| Phase | Description | Tasks |
|-------|-------------|-------|
| Phase 1 | Setup | T001-T003 |
| Phase 2 | Foundational | T004-T006 |
| Phase 3 | US1: Basic operations (P1) | T007-T013 |
| Phase 4 | US2: Batch operations (P1) | T014-T018 |
| Phase 5 | US3: Connection management (P2) | T019-T022 |
| Phase 6 | US4: Error handling (P2) | T023-T026 |
| Phase 7 | Polish & CI/CD | T027-T029 |

**Total Tasks**: 29
**MVP Scope**: Phase 1-3 (User Story 1) - Tasks T001-T013
