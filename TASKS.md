# ZeroKV 开发任务清单

版本: v0.1.0  
最后更新: 第1次更新

## 📊 任务概览

- **总任务数**: 11
- **已完成**: 0 (0%)
- **进行中**: 0
- **待开始**: 11 (100%)

## 🎯 里程碑任务

### #1 Milestone 1: 基础设施搭建
**状态**: ⏳ 待开始  
**优先级**: P0 (最高)  
**依赖**: 无

**包含任务**:
- Task #7: 实现 P2P UCX Mock 核心接口
- Task #8: 实现 UCX 控制服务器
- Task #9: 实现 UCX 控制客户端
- Task #10: 编写基础设施单元测试
- Task #11: 配置 CI/CD Pipeline

**验收标准**:
- ✅ P2P Mock层完整实现并通过测试
- ✅ UCX控制平面可建立连接
- ✅ 单元测试覆盖率 >80%
- ✅ CI/CD自动运行

---

### #2 Milestone 2: 核心KV功能实现
**状态**: 🔒 被阻塞  
**优先级**: P0  
**依赖**: Task #1 完成

**关键功能**:
- ZeroKVServer 实现 (Put/Delete/管理)
- ZeroKVClient 实现 (Get/BatchGet)
- P2P连接管理和复用
- 错误处理和重试机制

**验收标准**:
- ✅ 端到端集成测试通过
- ✅ 支持跨Device传输 (Mock模式)

---

### #3 Milestone 3: Python绑定开发
**状态**: 🔒 被阻塞  
**优先级**: P1  
**依赖**: Task #2 完成

**关键功能**:
- pybind11集成
- NumPy数组支持
- PyTorch Tensor支持
- Python异常处理

**验收标准**:
- ✅ Python包可通过pip安装
- ✅ Python示例运行成功
- ✅ 支持NumPy和PyTorch

---

### #4 Milestone 4: 性能优化
**状态**: 🔒 被阻塞  
**优先级**: P1  
**依赖**: Task #2 完成

**优化目标**:
- Get延迟 P95 < 50μs
- Get带宽 > 180GB/s
- QPS > 5M ops/s

**优化项**:
- 零拷贝路径优化
- 连接池优化
- 内存对齐和预分配
- 并发优化

---

### #5 Milestone 5: 监控系统集成
**状态**: 🔒 被阻塞  
**优先级**: P2  
**依赖**: Task #2 完成

**交付物**:
- PerformanceMonitor实现
- Prometheus Exporter
- Grafana Dashboard配置
- 告警规则

---

### #6 Milestone 6: 测试与发布
**状态**: 🔒 被阻塞  
**优先级**: P0  
**依赖**: Task #3, #4, #5 完成

**验收标准**:
- ✅ 代码覆盖率 ≥ 80%
- ✅ 性能达到目标
- ✅ 无内存泄漏 (Valgrind验证)
- ✅ v1.0.0发布

---

## 📋 详细任务列表

### Task #7: 实现 P2P UCX Mock 核心接口
**状态**: ⏳ 可开始 (无依赖)  
**优先级**: P0 ⭐  
**预计工时**: 3天  
**负责人**: 待分配

**需要实现的接口**:
```cpp
- P2PMockInit(bool useRDMA)
- P2PGetRootInfo(HcclRootInfo* rootInfo)
- P2PCommInitRootInfo(P2PComm* comm, HcclRootInfo* rootInfo)
- P2PSend(P2PComm comm, void* buf, size_t bytes, uint32_t rank)
- P2PRecv(P2PComm comm, void* buf, size_t bytes, uint32_t rank, void* stream)
```

**技术要点**:
- 使用UCX ucp_worker和ucp_ep
- HcclRootInfo存储UCX worker地址
- 支持RDMA和TCP模式
- 模拟NPU内存

**交付文件**:
- `src/common/p2p_ucx_mock.cpp`
- `src/common/p2p_ucx_mock.h`

---

### Task #8: 实现 UCX 控制服务器
**状态**: 🔒 被阻塞 (依赖 #7)  
**优先级**: P0  
**预计工时**: 4天  
**负责人**: 待分配

**需要实现**:
- UCXControlServer 类
- UCP Context 和 Worker 初始化
- Listener 和连接管理
- RPC 消息处理框架
- Put/Get/Delete 请求处理

**交付文件**:
- `src/common/ucx_control_server.cpp`
- `src/common/ucx_control_server.h`

---

### Task #9: 实现 UCX 控制客户端
**状态**: 🔒 被阻塞 (依赖 #7)  
**优先级**: P0  
**预计工时**: 3天  
**负责人**: 待分配

**需要实现**:
- UCXControlClient 类
- Endpoint 连接建立
- 同步/异步RPC调用
- Protobuf序列化
- 超时和重试

**交付文件**:
- `src/common/ucx_control_client.cpp`
- `src/common/ucx_control_client.h`

---

### Task #10: 编写基础设施单元测试
**状态**: 🔒 被阻塞 (依赖 #7, #8, #9)  
**优先级**: P0  
**预计工时**: 2天  
**负责人**: 待分配

**测试范围**:
- P2P Mock 接口测试
- UCX 控制平面测试
- 错误处理测试
- 边界条件测试

**目标覆盖率**: >80%

**交付文件**:
- `tests/unit/test_p2p_mock.cpp`
- `tests/unit/test_ucx_control.cpp`

---

### Task #11: 配置 CI/CD Pipeline
**状态**: ⏳ 可开始 (无依赖)  
**优先级**: P1  
**预计工时**: 1天  
**负责人**: 待分配

**配置内容**:
- GitHub Actions工作流
- 自动编译 (Debug/Release)
- 单元测试自动运行
- 代码覆盖率报告
- 静态分析 (clang-tidy)

**交付文件**:
- `.github/workflows/ci.yml`
- `.github/workflows/coverage.yml`
- `.clang-format`
- `.clang-tidy`

---

## 🚀 当前可开始的任务

### ✅ 立即可开始 (0依赖):

| 任务ID | 任务名称 | 优先级 | 工时 |
|--------|---------|--------|------|
| #7 | 实现 P2P UCX Mock 核心接口 | P0 ⭐ | 3天 |
| #11 | 配置 CI/CD Pipeline | P1 | 1天 |

**建议**: 优先启动 #7，因为 #8 和 #9 都依赖它。

### ⏳ 下一批任务 (需 #7 完成):

| 任务ID | 任务名称 | 依赖 | 工时 |
|--------|---------|------|------|
| #8 | 实现 UCX 控制服务器 | #7 | 4天 |
| #9 | 实现 UCX 控制客户端 | #7 | 3天 |

**并行**: #8 和 #9 可以在 #7 完成后并行开发。

### 🔒 第三批任务 (需 #7, #8, #9 完成):

| 任务ID | 任务名称 | 依赖 | 工时 |
|--------|---------|------|------|
| #10 | 编写基础设施单元测试 | #7, #8, #9 | 2天 |

---

## 📊 任务依赖关系图

```
#1 (Milestone 1)
  ├─ #7 (P2P Mock) ⭐ 可开始
  │   ├─ #8 (UCX Server)
  │   └─ #9 (UCX Client)
  │       └─ #10 (Unit Tests)
  └─ #11 (CI/CD) ⭐ 可开始

#2 (Milestone 2) [依赖 #1]
  ├─ #3 (Python) [依赖 #2]
  ├─ #4 (Performance) [依赖 #2]
  └─ #5 (Monitoring) [依赖 #2]
      └─ #6 (Release) [依赖 #3, #4, #5]
```

---

## 📈 进度追踪

| 里程碑 | 任务数 | 已完成 | 进度 | 状态 |
|--------|-------|--------|------|------|
| Milestone 1 | 5 | 0 | 0% | ⏳ 待开始 |
| Milestone 2 | - | 0 | 0% | 🔒 被阻塞 |
| Milestone 3 | - | 0 | 0% | 🔒 被阻塞 |
| Milestone 4 | - | 0 | 0% | 🔒 被阻塞 |
| Milestone 5 | - | 0 | 0% | 🔒 被阻塞 |
| Milestone 6 | - | 0 | 0% | 🔒 被阻塞 |
| **总计** | **11** | **0** | **0%** | - |

---

## 💡 任务执行指南

### 任务认领

1. 在 TASKS.md 中更新"负责人"字段
2. 提交Git commit记录认领
3. 创建对应的功能分支

```bash
# 认领 Task #7
git checkout -b feature/task-7-p2p-mock
git commit --allow-empty -m "task: claim #7 - P2P Mock implementation"
```

### 任务开发

```bash
# 1. 实现功能
vim src/common/p2p_ucx_mock.cpp

# 2. 定期提交
git add src/common/p2p_ucx_mock.cpp
git commit -m "feat(common): add P2PMockInit implementation"

# 3. 运行测试
./scripts/build.sh --debug
cd build && ctest
```

### 任务完成

每个任务完成需要满足：
- ✅ 代码实现完成
- ✅ 单元测试通过
- ✅ 代码覆盖率达标
- ✅ 代码审查通过
- ✅ 文档已更新
- ✅ Git提交规范

完成后更新任务状态：
```bash
# 更新 TASKS.md
# 将任务状态从 ⏳ 改为 ✅

git add TASKS.md
git commit -m "task: complete #7 - P2P Mock implementation"
git checkout main
git merge --no-ff feature/task-7-p2p-mock
```

---

## 🔗 相关文档

- [技术设计文档](docs/technical_design_document.md)
- [API参考](docs/api_reference.md)
- [开发排期](docs/development_schedule.md)
- [Git工作流](docs/GIT_SETUP_SUMMARY.md)

---

**版本**: v0.1.0  
**最后更新**: 第1次  
**下次更新**: 完成任何一个任务时
