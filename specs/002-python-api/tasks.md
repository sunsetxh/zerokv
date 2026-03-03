# Tasks: ZeroKV Python API 支持

**Input**: Design documents from `/specs/002-python-api/`
**Prerequisites**: spec.md

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup (基础检查)

**Purpose**: Verify current implementation status

- [X] T001 检查现有 pybind11 实现 in python/pybind.cc
- [X] T002 检查 Python 依赖配置 in python/setup.py
- [X] T003 检查 CMakeLists.txt Python 构建配置

---

## Phase 2: User Story 1 - 基础 Python API (Priority: P1) 🎯 MVP

**Goal**: Python 开发者能够导入 zerokv 并执行基本 put/get 操作

**Independent Test**: `import zerokv; client.put(key, value); client.get(key)`

### Implementation

- [X] T004 [P] [US1] 验证 `import zerokv` 能正常工作
- [X] T005 [P] [US1] 实现 zerokv.Client.connect() 方法
- [X] T006 [US1] 实现 zerokv.Client.put() 方法
- [X] T007 [US1] 实现 zerokv.Client.get() 方法
- [X] T008 [US1] 测试基础 put/get 功能

---

## Phase 3: User Story 2 - 批量操作 (Priority: P1)

**Goal**: 支持批量 put/get 操作，提高大规模参数同步效率

**Independent Test**: client.batch_put(items), client.batch_get(keys)

### Implementation

- [X] T009 [P] [US2] 实现 client.batch_put() 方法
- [X] T010 [P] [US2] 实现 client.batch_get() 方法
- [X] T011 [US2] 测试批量操作性能

---

## Phase 4: User Story 3 - 上下文管理器 (Priority: P2)

**Goal**: 支持 Python with 语句，自动管理连接

**Independent Test**: `with client:`

### Implementation

- [X] T012 [P] [US3] 实现 __enter__ 和 __exit__ 方法
- [X] T013 [US3] 测试上下文管理器功能

---

## Phase 5: User Story 4 - 连接池 (Priority: P2)

**Goal**: 支持连接池管理，提高并发性能

### Implementation

- [X] T014 [P] [US4] 实现连接池管理
- [X] T015 [US4] 测试连接池并发性能

---

## Phase 6: User Story 5 - 异步支持 (Priority: P3)

**Goal**: 支持 async/await 异步操作

### Implementation

- [X] T016 [P] [US5] 实现异步客户端类
- [X] T017 [US5] 测试异步操作

### Implementation

- [X] T021 [US1] 创建 pytest 测试用例 in python/tests/
- [X] T022 [US1] 运行测试验证基本功能

---

## Phase 7: 打包和发布

**Purpose**: 支持 pip install

### Implementation

- [X] T018 [P] 更新 setup.py 配置
- [X] T019 [P] 创建 pyproject.toml
- [X] T020 [US1] 测试 pip install 安装

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies
- **US1 (Phase 2)**: Depends on Setup
- **US2 (Phase 3)**: Depends on US1
- **US3 (Phase 4)**: Depends on US1
- **US4 (Phase 5)**: Depends on US1
- **US5 (Phase 6)**: Depends on US1
- **Polish (Phase 7)**: Depends on all US complete

### User Story Dependencies

- **US1 (P1)**: 基础，必须优先完成
- **US2 (P1)**: 依赖于 US1
- **US3 (P2)**: 依赖于 US1
- **US4 (P2)**: 依赖于 US1
- **US5 (P3)**: 依赖于 US1

---

## Parallel Opportunities

- T004, T005, T006, T007 可并行 (US1 实现)
- T009, T010 可并行 (US2)
- T012, T013 可并行 (US3)
- T018, T019 可并行 (打包配置)

---

## Implementation Strategy

### MVP First (US1 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: US1 - Basic Python API
3. **STOP and VALIDATE**: Python 开发者能导入并使用 zerokv

### Incremental Delivery

1. Setup → 环境就绪
2. US1 → 基础 API 可用
3. US2 → 批量操作可用
4. US3 → 上下文管理器可用
5. US4 → 连接池可用
6. US5 → 异步支持可用
7. Polish → 打包发布

---

## Notes

- 使用 pybind11 生成 Python 绑定
- 需要确保 UCX 依赖正确链接
- 测试需要运行中的 ZeroKV server
- Python 3.8+ 版本支持
