# Tasks: ZeroKV SDK 集成支持

**Input**: Design documents from `/specs/003-sdk-integration/`
**Prerequisites**: plan.md, spec.md

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup (基础检查)

**Purpose**: Verify current implementation status

- [X] T001 检查现有 server.h 实现 in include/zerokv/server.h
- [X] T002 检查现有 client.h 实现 in include/zerokv/client.h
- [X] T003 检查 CMakeLists.txt 现状

---

## Phase 2: CMake SDK 支持

**Purpose**: Enable find_package integration

- [X] T004 创建 CMake/zerokv-config.cmake.in 模板
- [X] T005 添加 find_package 支持到 CMakeLists.txt
- [X] T006 配置静态库输出 (libzerokv.a)
- [X] T007 配置动态库输出 (libzerokv.so)
- [X] T008 [P] 验证 CMake 构建成功

---

## Phase 3: User Story 1 - 嵌入式Server (Priority: P1) 🎯 MVP

**Goal**: 用户代码可以嵌入Server

**Independent Test**: 编译用户代码 include zerokv/server.h 并调用 start()

### Implementation

- [X] T009 [P] [US1] 验证 server.start() 在 examples 中可用
- [X] T010 [P] [US1] 创建嵌入式Server示例 in examples/embedded_server.cc
- [X] T011 [US1] 测试嵌入式Server编译和运行

---

## Phase 4: User Story 2 - 嵌入式Client (Priority: P1)

**Goal**: 用户代码可以使用Client API

**Independent Test**: 编译用户代码 include zerokv/client.h 并调用 put/get

### Implementation

- [X] T012 [P] [US2] 验证 client.put/get 在 examples 中可用
- [X] T013 [P] [US2] 创建嵌入式Client示例 in examples/embedded_client.cc
- [X] T014 [US2] 测试嵌入式Client编译和运行

---

## Phase 5: User Story 3 - 静态/动态库 (Priority: P2)

**Goal**: 支持静态/动态链接

**Independent Test**: 用户项目通过 find_package(zerokv) 链接

### Implementation

- [X] T015 [P] [US3] 验证 libzerokv.a 生成
- [X] T016 [P] [US3] 验证 libzerokv.so 生成
- [X] T017 [US3] 创建 CMake 使用示例 in examples/cmake/

---

## Phase 6: Polish & 文档

**Purpose**: 完善SDK文档和示例

- [X] T018 [P] 更新 docs/DEVELOPMENT.md 添加SDK使用说明
- [X] T019 [P] 创建 API 文档 in docs/api/
- [X] T020 验证 10行内集成Server (SC-001)
- [X] T021 验证 10行内使用Client (SC-002)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies
- **CMake (Phase 2)**: Depends on Setup - 完成后其他 phases 可并行
- **US1 (Phase 3)**: Depends on CMake
- **US2 (Phase 4)**: Depends on CMake
- **US3 (Phase 5)**: Depends on CMake
- **Polish (Phase 6)**: Depends on all US complete

### User Story Dependencies

- **US1 (P1)**: Can start after CMake - 独立
- **US2 (P1)**: Can start after CMake - 独立
- **US3 (P2)**: Can start after CMake - 独立

---

## Parallel Opportunities

- T004, T005, T006, T007 可并行 (CMake配置)
- T009, T010 可并行 (US1)
- T012, T013 可并行 (US2)
- T015, T016 可并行 (US3)
- T018, T019 可并行 (Polish)

---

## Implementation Strategy

### MVP First (US1 Only)

1. Complete Phase 1-2: Setup + CMake
2. Complete Phase 3: US1 - Embedded Server
3. **STOP and VALIDATE**: 用户可以嵌入Server

### Incremental Delivery

1. Setup + CMake → SDK基础就绪
2. US1 → 嵌入式Server可用
3. US2 → 嵌入式Client可用
4. US3 → 静态/动态库支持
5. Polish → 文档完善

---

## Notes

- 当前 server.h 和 client.h 已存在，主要需要CMake配置
- 需要确保examples可以编译运行
- CMake find_package 支持是关键需求
