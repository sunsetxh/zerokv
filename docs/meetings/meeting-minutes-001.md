# 会议纪要 #001：需求澄清与方案设计

- **日期**：2026-03-04
- **参与角色**：PM(产品经理)、Arch(技术架构师)、Dev(开发工程师)、QA(测试工程师)、Coord(项目协调员)
- **会议阶段**：需求澄清 → 方案设计 → 总结输出

---

## 一、项目定位

> 面向AI训练和推理场景的统一AXON传输库，基于UCX实现零SM消耗的高性能数据传输，同时支持NVIDIA GPU和华为Ascend NPU。

核心差异化：不做NCCL替代品，而是**统一传输层 + 集合通信Plugin扩展**。

---

## 二、适用场景（PM调研）

| 场景 | Value大小 | 核心需求 |
|------|----------|---------|
| KV Cache分离推理传输 | 256KB~100MB | 零SM消耗、非连续内存scatter/gather |
| AI分布式训练梯度同步 | 64KB~1GB | 高吞吐、多路径RDMA |
| 参数服务器/模型并行 | 1MB~1GB | GPU Direct RDMA |
| HPC halo exchange | 1KB~64KB | 极致低延迟 |
| RLHF权重同步 | 100MB~1GB | 异步非阻塞、Python友好 |

### 目标用户

1. **AI基础设施工程师** — 痛点：NCCL/HCCL双栈维护、AXON传输消耗GPU SM
2. **推理系统开发者** — 痛点：KV Cache传输方案碎片化、需Python+C++双接口
3. **ML框架开发者** — 痛点：UCX-Py已停止维护、需与NCCL/HCCL无缝协作
4. **HPC研究人员** — 痛点：MPI接口老旧、需更底层控制

### 竞品对比

| 维度 | NCCL | HCCL | Gloo | UCX/UCXX | NIXL | Mooncake TE | **本项目** |
|------|------|------|------|----------|------|-------------|-----------|
| 硬件支持 | NVIDIA only | Ascend only | CPU为主 | 多硬件 | NVIDIA为主 | 多硬件 | **双栈** |
| 通信类型 | Collective+AXON | Collective+AXON | Collective | AXON+RMA | AXON | AXON | **AXON+Plugin扩展** |
| GPU SM消耗 | AXON消耗SM | 类似 | N/A | 零 | 零 | 零 | **零SM** |
| KV Cache优化 | 无 | 无 | 无 | 无 | 专门优化 | 专门优化 | **内建支持** |
| 非连续内存 | 有限 | 有限 | 无 | 支持 | 支持 | 支持 | **一等公民** |

---

## 三、架构设计（Arch设计，Dev验证）

### 分层架构

```
┌─────────────────────────────────────────────────┐
│  Python API (nanobind + asyncio)                │  python/axon/
├─────────────────────────────────────────────────┤
│  C++ Public API                                 │  include/axon/
│  Context │ Worker │ Endpoint │ Future<T>        │
├─────────────────────────────────────────────────┤
│  Plugin Layer (NCCL / HCCL / Custom)            │  plugin/plugin.h
│  CollectivePlugin + PluginRegistry              │
├─────────────────────────────────────────────────┤
│  Memory Management                              │  memory.h
│  MemoryRegion │ MemoryPool(slab) │ RegCache(LRU)│
├─────────────────────────────────────────────────┤
│  Transport Abstraction                          │
│  UCX(UCP) │ ProtocolSelector(eager/rndv)        │
├─────────────────────────────────────────────────┤
│  UCX Foundation (UCP API)                       │
└─────────────────────────────────────────────────┘
```

### UCX技术调研要点

- **选择UCP层**：自动处理eager/rendezvous切换、multi-rail、传输选择
- **CUDA GPU**：UCX >= 1.8原生支持，需`cuda_copy,cuda_ipc`
- **Ascend NPU**：UCX原生不支持，需通过HCCL Plugin集成（**关键风险点**）
- **线程安全**：避免`THREAD_MODE_MULTI`（giant lock），采用`SINGLE`模式每线程一个worker

---

## 四、关键技术决策

| # | 决策项 | 选择 | 理由 | 决策者 |
|---|--------|------|------|--------|
| 1 | UCX API层级 | **UCP** | 自动eager/rndv切换、multi-rail、传输自动选择 | Arch |
| 2 | 线程模型 | **每线程一个Worker (SINGLE模式)** | 零锁开销、线性扩展、UCX最佳实践 | Arch+Dev |
| 3 | Eager/Rndv阈值 | **Host: 256KB, GPU: 4KB, auto可选** | 匹配UCX默认，GPU避免bounce copy | Arch |
| 4 | 内存管理 | **Slab MemoryPool + LRU RegistrationCache** | 消除热路径注册开销，命中率>95% | Arch+Dev |
| 5 | 连接管理 | **Lazy connect + EndpointPool** | 按需建立、复用连接、空闲回收 | Arch |
| 6 | Ascend NPU支持 | **Plugin方式集成HCCL** | UCX原生不支持NPU内存 | Arch |
| 7 | Python绑定 | **nanobind** | 编译快3x、运行快3-10x、体积小3-5x vs pybind11 | Arch |
| 8 | 异步模型(C++) | **Future\<T\> + callback** | 支持阻塞/非阻塞/链式三种模式 | Dev |
| 9 | 异步模型(Python) | **asyncio + event_fd** | worker的event_fd注册到event loop自动progress | Dev |
| 10 | GIL处理 | **所有C++ blocking调用释放GIL** | GIL开销~100ns vs RDMA ~2us，影响<5% | Dev |
| 11 | 构建系统 | **CMake 3.20+, C++20** | UCX/CUDA/NCCL生态最好 | Arch |
| 12 | 日志 | **spdlog** | 异步、高性能、header-only | Arch |
| 13 | 测试框架 | **Google Test + Google Benchmark + pytest** | 业界标准 | QA |
| 14 | 性能overhead目标 | **>=64KB: <5%, <4KB: <2us额外延迟** | 与原生UCX对比 | QA |
| 15 | 序列化 | **核心不依赖；控制消息可选flatbuffers** | 传输库操作raw buffer | Arch |

---

## 五、接口设计（Dev输出）

### C++ API核心抽象

```
Config::Builder  →  链式配置 + from_env()覆盖
Context          →  shared_ptr语义，顶层资源句柄
Worker           →  progress引擎，每线程一个
Endpoint         →  tag_send/recv, put/get, stream, atomic
Future<T>        →  ready()/get()/then()/on_complete()
MemoryRegion     →  注册/分配一体化，暴露remote_key()
MemoryPool       →  预注册slab分配器
RegistrationCache → LRU + 区间树
```

### Python API特点

- `Config(**kwargs)` 替代Builder模式
- 所有传输操作为`async def`，实现`__await__`协议
- 支持buffer protocol零拷贝（numpy/cupy/memoryview/bytearray）
- `Worker.attach_to_event_loop()` 自动驱动UCX progress

### Plugin接口

- `CollectivePlugin`抽象类：allreduce/broadcast/allgather/reduce_scatter/alltoall/send/recv
- 注册方式：静态链接 / `dlopen`动态加载 / 目录自动扫描
- 导出C工厂函数：`axon_plugin_create()`

### 关键实现挑战方案

| 挑战 | 方案 |
|------|------|
| UCX Worker线程安全 | 每线程一个Worker + MPSC无锁队列跨线程提交 |
| 大消息零拷贝 | UCX rendezvous + 预注册MemoryRegion |
| Python GIL | 所有C++ blocking调用`gil_scoped_release` + event_fd驱动 |
| Registration Cache | LRU + 区间树 O(log n)查找，支持部分重叠合并 |

---

## 六、性能规格目标（PM+QA共定）

### 节点间（单链路 200Gbps RoCE/IB）

| 消息大小 | 延迟目标 | 吞吐目标 | 典型场景 |
|---------|---------|---------|---------|
| 1KB | < 3us | > 300 MB/s(聚合) | HPC控制消息 |
| 64KB | < 10us | > 5 GB/s | 梯度分片 |
| 1MB | < 50us | > 20 GB/s | KV Cache页 |
| 100MB | < 5ms | > 23 GB/s(~线速) | 大梯度同步 |
| 1GB | < 45ms | > 23 GB/s | 模型权重传输 |

### 多路径聚合（4x200Gbps）

| 消息大小 | 吞吐目标 |
|---------|---------|
| 1MB | > 40 GB/s |
| 100MB | > 80 GB/s |
| 1GB | > 85 GB/s |

### 关键SLA

- GPU传输100%零拷贝（GPUDirect RDMA）
- AXON传输期间GPU SM占用 = 0
- 连接建立 < 100ms
- P99尾延迟 < 3x平均延迟
- CPU开销 < 5%（单核）

---

## 七、测试策略（QA输出）

### 测试分层

| 层级 | 框架 | 硬件依赖 | 触发时机 |
|------|------|---------|---------|
| 单元测试 | GTest + GMock | 无(Mock UCX) | 每次提交 |
| 集成测试 | GTest + pytest | SoftRoCE(Tier1), 真实RDMA(Tier2) | 每次提交/每日 |
| 性能测试 | Google Benchmark | RDMA NIC + GPU | 每日 |
| 压力测试 | 自定义 | 多节点集群 | 发布前 |

### 性能测试方法

- **延迟**：Ping-Pong，预热1000次，迭代10万次，报告p50/p95/p99
- **吞吐**：Streaming滑动窗口，总量>=1GB
- **回归检测**：Welch's t-test统计方法，避免噪声误报

### 测试矩阵（按优先级分级）

- P0：{1KB,64KB,1MB,1GB} × {rc,shm} × {Host,CUDA} × {1,8并发}
- P1：扩展消息大小和传输协议
- P2：全量组合

### 故障场景

18个故障场景(FAULT-001~018)：连接断开、对端crash、内存不足、网络抖动、GPU OOM等

### 质量门禁

- 代码行覆盖 >= 80%，分支覆盖 >= 70%
- 大消息带宽回退 <= 3%
- 小消息延迟增加 <= 10% (或0.5us绝对值)

---

## 八、需求优先级（MoSCoW）

### Must Have（第一期必须）

| # | 功能 |
|---|------|
| M1 | 基于UCX的AXON send/recv和RMA put/get |
| M2 | CUDA GPU内存直接传输（GPUDirect RDMA） |
| M3 | InfiniBand / RoCE RDMA传输后端 |
| M4 | C++核心API（同步+异步） |
| M5 | Python绑定（nanobind） |
| M6 | 零SM消耗的AXON传输 |
| M7 | 连接管理和端点发现 |
| M8 | 错误处理和超时机制 |
| M9 | 1KB-1GB全范围消息大小支持 |

### Should Have（第二期）

S1 非连续内存scatter/gather、S2 多路径RDMA、S3 NCCL Plugin、S4 HCCL Plugin、S5 GPU-NIC拓扑发现、S6 Registration Cache、S7 异步progress引擎、S8 批量传输API

### Could Have（第三期）

C1 TCP fallback、C2 共享内存优化、C3 NVMe-oF存储、C4 传输压缩、C5 可观测性、C6 自适应协议选择、C7 Collective通信原语

### Won't Have

W1 跨数据中心、W2 AMD ROCm、W3 容错弹性训练、W4 图编排调度

---

## 九、分期路线图

### 第一期 MVP

- 核心AXON传输：tag send/recv + RMA put/get
- Host Memory + RDMA(IB/RoCE) + TCP
- C++ API（同步+异步）
- 基本错误处理
- 性能达到规格表80%

### 第二期

- CUDA GPU Memory（GPUDirect RDMA）
- Python API（nanobind + asyncio）
- NCCL Plugin
- MemoryPool + RegistrationCache
- 非连续内存scatter/gather

### 第三期

- Ascend NPU（HCCL Plugin）
- 多路径RDMA带宽聚合
- 自动重连/故障恢复
- 连接池、内存池高级特性
- 可观测性（metrics/tracing）

---

## 十、已产出代码资产

```
axon/
├── CMakeLists.txt                          # 构建配置 (95行)
├── include/axon/
│   ├── common.h                            # ErrorCode, Status, Tag, MemoryType (146行)
│   ├── config.h                            # Config::Builder + Context (149行)
│   ├── memory.h                            # MemoryRegion/Pool/Cache (182行)
│   ├── future.h                            # Request + Future<T> (152行)
│   ├── worker.h                            # Worker + Listener (144行)
│   ├── endpoint.h                          # Endpoint: tag/RMA/stream/atomic (162行)
│   ├── plugin/plugin.h                     # CollectivePlugin + Registry (254行)
│   └── axon.h                               # 聚合头文件 (12行)
├── src/
│   ├── transport/ucx_impl_notes.h          # 实现技术方案 (264行)
│   ├── python/bindings.cpp                 # nanobind绑定 (367行)
│   └── plugin/nccl_plugin.cpp              # NCCL Plugin骨架 (229行)
├── python/axon/
│   ├── __init__.py                         # 包入口 (68行)
│   └── _core.pyi                           # 类型stub (496行)
├── examples/
│   ├── cpp_usage.cpp                       # 6个C++示例 (222行)
│   └── python_usage.py                     # 8个Python示例 (263行)
└── docs/
    └── test-strategy.md                    # 完整测试策略 (854行)
```

**总计：4059行**

---

## 十一、风险与待办

| # | 风险/待办 | 负责人 | 优先级 |
|---|----------|--------|--------|
| 1 | Ascend NPU的UCX支持确认 — UCX原生不支持，需验证HCCL Plugin可行性 | Arch | 高 |
| 2 | nanobind对CUDA array interface支持验证 | Dev | 中 |
| 3 | CI环境RDMA硬件 — Tier2/3需自托管Runner，SoftRoCE覆盖Tier1 | QA | 中 |
| 4 | 项目命名 | PM | 低 |
| 5 | 开源许可证选择（建议Apache 2.0） | PM | 低 |

---

## 十二、参考资料

- [UCCL: Efficient Communication Library for GPUs](https://github.com/uccl-project/uccl)
- [NIXL: NVIDIA Inference Xfer Library](https://github.com/ai-dynamo/nixl)
- [Mooncake Transfer Engine](https://github.com/kvcache-ai/Mooncake)
- [NCCL 2.27 Blog](https://developer.nvidia.com/blog/enabling-fast-inference-and-resilient-training-with-nccl-2-27/)
- [ICCL: GPU Training Communication](https://arxiv.org/html/2510.00991v1)
- [UCX OpenUCX FAQ](https://openucx.readthedocs.io/en/master/faq.html)
- [UCX Rendezvous Protocol](https://github.com/openucx/ucx/wiki/Rendezvous-Protocol)
- [UCX NVIDIA GPU Support](https://github.com/openucx/ucx/wiki/NVIDIA-GPU-Support)
- [UCX Thread Safety Research](https://link.springer.com/article/10.1007/s11227-024-06201-x)
- [nanobind Benchmarks](https://nanobind.readthedocs.io/en/latest/benchmark.html)
- [KV Cache Transfer Engine Deep Dive](https://uccl-project.github.io/posts/kv-transfer-engine/)

---

**会议结论**：需求澄清和方案设计阶段完成，各角色输出一致，无重大分歧。建议进入第一期MVP开发。

**下次会议议题**：第一期MVP详细排期与任务拆解。
