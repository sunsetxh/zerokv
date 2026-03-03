# Feature Specification: ZeroKV SDK 集成支持

**Feature Branch**: `003-sdk-integration`
**Created**: 2026-03-03
**Status**: Spec Complete
**Input**: User description: "支持server和client作为sdk在用户代码里集成"

## 当前架构分析

### 现有模块

| 模块 | 位置 | 说明 |
|------|------|------|
| Storage Engine | `src/storage/` | LRU内存池，支持1KB-1GB值 |
| Cache | `src/cache/` | LRU/LFU/FIFO/Random/TTL |
| Client | `src/client/` | 连接池、重试机制 |
| Server | `src/server/` | 独立进程运行 |
| Transport | `src/transport/` | UCX网络传输 |
| Python | `python/` | pybind11绑定(基础) |
| Metrics | `src/metrics/` | Counter/Gauge/Histogram |
| Config | `src/config/` | 配置管理 |

### 当前使用模式

1. **独立进程模式**: 编译为 `zerokv_server` 可执行文件运行
2. **Python模式**: 需要单独安装C++库后pip安装Python包

### 不足之处

- 用户无法将Server/Client直接嵌入自己的应用程序
- 缺乏头文件形式的SDK供C++应用直接链接
- 需要进程间通信，无法本地调用

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - 嵌入式Server (Priority: P1)

As an AI Developer, I want to embed ZeroKV server in my training code, so I can share parameters without running separate processes.

**Why this priority**: Eliminates process management overhead, enables tighter integration.

**Independent Test**: User's training code can start/stop embedded server.

**Acceptance Scenarios**:

1. **Given** C++ application, **When** include zerokv/server.h and call server.start(), **Then** server starts listening
2. **Given** running embedded server, **When** call server.stop(), **Then** server shuts down cleanly

---

### User Story 2 - 嵌入式Client (Priority: P1)

As an AI Developer, I want to use ZeroKV client in my application, so I can connect to any ZeroKV server.

**Why this priority**: Direct API access without wrapper processes.

**Independent Test**: Application can perform put/get operations.

**Acceptance Scenarios**:

1. **Given** C++ application with client, **When** call client.put(key, value), **Then** value stored in server
2. **Given** stored key, **When** call client.get(key), **Then** returns stored value

---

### User Story 3 - 静态/动态库 (Priority: P2)

As a Developer, I want to link ZeroKV as a library, so I can distribute my application easily.

**Why this priority**: Simplifies deployment, reduces dependencies.

**Independent Test**: Application links against libzerokv.a or libzerokv.so.

**Acceptance Scenarios**:

1. **Given** CMake project, **When** find_package(zerokv), **Then** can link zerokv library
2. **Given** linked library, **When** run application, **Then** ZeroKV functions work correctly

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: 提供 `zerokv/server.h` 头文件，支持嵌入式Server
- **FR-002**: 提供 `zerokv/client.h` 头文件，支持嵌入式Client
- **FR-003**: Server支持start/stop生命周期管理
- **FR-004**: Client支持连接/断开生命周期管理
- **FR-005**: CMake支持 `find_package(zerokv)` 方式集成
- **FR-006**: 支持静态链接 (libzerokv.a)
- **FR-007**: 支持动态链接 (libzerokv.so)
- **FR-008**: 提供C++ API文档和使用示例

### Key Entities

- **EmbeddedServer**: 嵌入式服务器实例
- **EmbeddedClient**: 嵌入式客户端实例
- **ServerConfig**: 服务器配置
- **ClientConfig**: 客户端配置

---

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 用户代码能在10行内集成ZeroKV Server
- **SC-002**: 用户代码能在10行内使用ZeroKV Client
- **SC-003**: 嵌入式模式延迟 < 1ms (vs 进程间通信)
- **SC-004**: 支持Linux/macOS/Windows三平台

---

## Assumptions

- 使用CMake作为构建系统
- 需要先编译ZeroKV库
- 用户需要C++17兼容的编译器
