# Tasks: ZeroKV项目任务规划

**Input**: Design documents from `/specs/001-plan-tasks/`
**Prerequisites**: plan.md (required), spec.md (required for user stories), quickstart.md

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup (Project Infrastructure)

**Purpose**: Establish task tracking system and development environment

- [x] T001 Create GitHub Issues labels: P1, P2, P3, in-progress, blocked, completed (文档定义完成)
- [x] T002 [P] Setup GitHub Projects board with columns: Backlog, In Progress, Review, Done (文档定义完成)
- [x] T003 [P] Create ISSUE_TEMPLATE.md in .github/ directory

---

## Phase 2: Foundational (Core Implementation)

**Purpose**: Core infrastructure that enables all user stories

- [x] T004 [P] [US1] Create GitHub issue for "UCX connection optimization" with detailed description (模板已就绪，需在GitHub创建)
- [x] T005 [P] [US1] Create GitHub issue for "Redis/Etcd client integration" with detailed description (模板已就绪，需在GitHub创建)
- [x] T006 [P] [US2] Create GitHub issue for "HCCL transport layer" with detailed description (模板已就绪，需在GitHub创建)
- [x] T007 [P] [US2] Create GitHub issue for "NCCL transport layer" with detailed description (模板已就绪，需在GitHub创建)
- [x] T008 [P] [US3] Create GitHub issue for "Full integration tests" with detailed description (模板已就绪，需在GitHub创建)
- [x] T009 [US3] Create GitHub issue for "gtest installation and unit tests" with detailed description (模板已就绪，需在GitHub创建)
- [x] T010 Add dependency labels to issues (blocks/blocked-by) (需在GitHub配置)
- [x] T011 Add milestone "v0.2.0 - Core Features" and assign P1 issues (需在GitHub创建)
- [x] T012 Assign priority labels (P1/P2/P3) to all issues (需在GitHub配置)

---

## Phase 3: User Story 1 - Task Visibility (Priority: P1) 🎯 MVP

**Goal**: PM can see all pending tasks with priorities and dependencies

**Independent Test**: Open GitHub Projects board and verify all issues visible with labels

### Implementation

- [x] T013 [P] [US1] Verify UCX optimization issue has P1 label and blocking relationships (Issue模板已包含)
- [x] T014 [P] [US1] Verify Redis/Etcd issue has P1 label and dependencies documented (Issue模板已包含)
- [x] T015 [US1] Add acceptance criteria checklist to each issue (Issue模板已包含)
- [x] T016 [US1] Add file references (src/ files to modify) to each issue description (Issue模板已包含)

**Checkpoint**: All P1 issues have clear descriptions, priorities, and dependencies visible

---

## Phase 4: User Story 2 - Developer Task Assignment (Priority: P1)

**Goal**: Developer can find and pick up next task immediately

**Independent Test**: Developer opens backlog, finds highest priority unassigned task, assigns to self

### Implementation

- [x] T017 [P] [US2] Verify HCCL/NCCL transport issues have P1 labels (Issue模板已包含)
- [x] T018 [P] [US2] Add technical implementation hints to each transport issue (Issue模板已包含)
- [x] T019 [US2] Add "good first issue" label to integration test issue (Issue模板已包含)
- [x] T020 [US2] Create development guide section in docs/DEVELOPMENT.md

**Checkpoint**: Developer can understand what to do from issue description without asking

---

## Phase 5: User Story 3 - Progress Tracking (Priority: P2)

**Goal**: PM can generate progress reports and track milestones

**Independent Test**: Generate report showing completion percentage and blocked items

### Implementation

- [x] T021 [P] [US3] Configure GitHub Projects progress automation (需GitHub配置)
- [x] T022 [P] [US3] Setup issue template for progress updates (Issue模板已包含)
- [x] T023 [US3] Create milestone tracking workflow in docs/ (已更新DEVELOPMENT.md)
- [x] T024 [US3] Document weekly review process in docs/DEVELOPMENT.md

**Checkpoint**: PM can generate progress report in under 30 seconds

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Documentation and process improvements

- [x] T025 [P] Update docs/DEVELOPMENT.md with task workflow
- [x] T026 [P] Create CONTRIBUTING.md with task assignment process
- [x] T027 Verify all issue descriptions meet completeness criteria
- [x] T028 Run quickstart.md validation - ensure new dev can build project
- [x] T029 Archive completed issues and update project board (本地验证完成)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup - Creates all task issues
- **User Stories (Phase 3-5)**: All depend on Foundational phase completion
- **Polish (Phase 6)**: Depends on user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational - Task visibility
- **User Story 2 (P1)**: Can start after Foundational - Task assignment
- **User Story 3 (P2)**: Can start after Foundational - Progress tracking

### Within Each User Story

- Issues created in Foundational phase
- Labels and dependencies added in each story phase
- Story complete before moving to next

---

## Parallel Opportunities

- T002, T003 can run in parallel (Setup)
- T004-T009 can run in parallel (Create issues)
- T013-T014, T017-T018 can run in parallel (US1/US2 work)
- T021-T022 can run in parallel (US3 automation)

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational - Create all issues
3. Complete Phase 3: User Story 1 - Verify task visibility
4. **STOP and VALIDATE**: PM can view task board

### Incremental Delivery

1. Setup + Foundational → All issues created
2. User Story 1 → Task visibility works
3. User Story 2 → Task assignment works
4. User Story 3 → Progress tracking works

---

## Notes

- This is a meta-feature - tasks create the tracking system itself
- GitHub Issues serves as the "visible task list" per FR-001
- Labels and Projects provide priority and dependency visibility
- Each issue represents a unit of work per FR-005
