# P2P 高性能传输库 — 技术调研与架构设计

> 角色: 技术架构师 (Arch)
> 日期: 2026-03-04

---

## 一、UCX 技术调研

### 1.1 UCP / UCT / UCS 三层 API 分析与推荐

UCX 框架由三个公开 API 层组成，每层可作为独立库使用：

```
+----------------------------------------------------------+
|  UCP (UC-Protocols)   -- 高层通信协议                      |
|  tag-matching, RMA, AM, stream, collectives              |
+----------------------------------------------------------+
|  UCT (UC-Transports)  -- 低层传输抽象                      |
|  直接访问硬件网络功能, 零拷贝, 原子操作                      |
+----------------------------------------------------------+
|  UCS (UC-Services)    -- 基础服务                          |
|  内存池, 数据结构, 平台抽象, 线程原语                        |
+----------------------------------------------------------+
```

| 层级 | 适用场景 | 优点 | 缺点 |
|------|---------|------|------|
| **UCP** | MPI/PGAS 编程模型、通用应用层通信 | 自动传输选择、消息分片、multi-rail、eager/rendezvous 自动切换 | 抽象开销略高 |
| **UCT** | 极致性能的定制传输、自研协议栈 | 直接硬件访问、最低延迟 | 需自行处理分片、传输选择、连接管理 |
| **UCS** | 内部工具库 | 高效内存池、数据结构 | 非通信层，通常不单独使用 |

**推荐：基于 UCP 层开发。** 理由如下：
1. UCP 自动处理 eager/rendezvous 协议切换，正好匹配项目 1KB-1GB 的 value 范围
2. UCP 内置 multi-rail 和传输自动选择（IB/RoCE/SHM/TCP），省去大量底层适配代码
3. UCP 的 tag-matching 和 Active Message API 可直接映射到 P2P KV 传输语义
4. UCT 层适合做极致优化时的逃逸通道，可通过 `native_handle()` 暴露

### 1.2 传输协议支持情况

| 传输协议 | UCX_TLS 标识 | 延迟 | 带宽 | 说明 |
|---------|-------------|------|------|------|
| **InfiniBand RC** | `rc`, `rc_x` | ~1us | ~200Gbps (HDR) | 可靠连接，最优大消息 |
| **InfiniBand UD** | `ud`, `ud_x` | ~1us | ~200Gbps | 不可靠数据报，可扩展性好 |
| **RoCE** | `rc`, `ud` (over RoCE NIC) | ~2-5us | ~100-200Gbps | RDMA over Ethernet，需无损网络 |
| **Shared Memory** | `shm`, `posix`, `sysv`, `cma`, `knem`, `xpmem` | ~100ns | 内存带宽 | 同节点进程间，延迟最低 |
| **TCP** | `tcp` | ~10-50us | ~10-100Gbps | 回退方案，总是可用 |

通过 `UCX_TLS` 环境变量或 `ucp_config` 可控制传输选择。默认情况下 UCX 自动探测并选择最优传输。

### 1.3 GPU / NPU 内存支持

**CUDA GPU 内存**：
- UCX >= 1.8 支持 CUDA 内存指针直接用于 tag send/recv、stream、active message API
- 需要 CUDA >= 6.0，推荐配合 `nv_peer_mem` 或 `nvidia-peermem` 内核模块
- 关键配置：`UCX_MEMTYPE_REG_WHOLE_ALLOC_TYPES=cuda` 启用基于 base address 的注册缓存
- `UCX_TLS` 显式指定时必须包含 `cuda_copy,cuda_ipc` 才能识别 GPU 内存
- RMA API 对 GPU 内存支持不完整，需注意

**ROCm AMD GPU**：
- ROCm fork 提供 `rocm_copy`, `rocm_ipc` 传输
- 用法与 CUDA 类似

**Ascend NPU**：
- **UCX 原生不支持华为 Ascend NPU 内存**
- 本项目需要通过 **Plugin 机制** 集成 HCCL，而非依赖 UCX 直接访问 NPU 内存
- 可行方案：(1) NPU 内存先拷贝到 Host 再走 UCX；(2) 通过 Plugin 直接调用 HCCL API 完成 NPU 间传输

### 1.4 线程安全模型

UCX 提供三种线程模式：

| 模式 | 含义 | 性能 |
|------|------|------|
| `UCS_THREAD_MODE_SINGLE` | 单线程访问 worker | 最优，无锁开销 |
| `UCS_THREAD_MODE_SERIALIZED` | 多线程可访问但需外部串行化 | 中等 |
| `UCS_THREAD_MODE_MULTI` | 多线程并发访问，UCX 内部加锁 | 最差（giant lock） |

**最佳实践：**
- 避免使用 `THREAD_MODE_MULTI`，其内部使用 giant lock，性能差
- **推荐方案：每线程一个 worker（SINGLE 模式）**，线程与 CPU 核绑定
- 用户回调在 `ucp_worker_progress()` 上下文中执行，必须快速返回
- 创建 worker 后应通过 `ucp_worker_query()` 验证实际线程模式

---

## 二、整体架构设计

### 2.1 分层架构图

```
+============================================================================+
|                        Python Binding Layer                                 |
|  nanobind wrapper | async support (asyncio) | numpy/torch tensor interop   |
+============================================================================+
|                          C++ Public API Layer                               |
|  Context | Worker | Endpoint | send/recv/put/get | Future<T>               |
+============================================================================+
|                          Plugin Layer (NCCL/HCCL)                           |
|  PluginRegistry | CollectivePlugin | NCCLPlugin | HCCLPlugin               |
+============================================================================+
|                        Memory Management Layer                              |
|  MemoryRegion | MemoryPool (slab) | RegistrationCache (LRU)               |
+============================================================================+
|                       Transport Abstraction Layer                           |
|  TransportBackend (interface) | UcxTransport | TcpTransport | ShmTransport |
|  ConnectionManager | EndpointPool | ProtocolSelector (eager/rndv)          |
+============================================================================+
|                          UCX Foundation Layer                               |
|  ucp_context | ucp_worker | ucp_ep | ucp_mem | Tag/AM/RMA primitives      |
|  UCS: memory pools, lock-free queues, timers, logging                      |
+============================================================================+
|                       Hardware / OS / Drivers                               |
|  InfiniBand | RoCE | CUDA/ROCm/Ascend | Shared Memory | TCP/IP stack      |
+============================================================================+
```

### 2.2 各层职责与关键接口

#### Layer 1: UCX Foundation Layer

不对外暴露，作为 UcxTransport 的内部实现。

```cpp
// 内部类，不在公开头文件中
class UcxContext {
    ucp_context_h ctx_;
    ucp_config_t* config_;
public:
    Status init(const Config& cfg);
    ucp_context_h handle() const;
};

class UcxWorker {
    ucp_worker_h worker_;
public:
    Status progress();
    Status wait_for_events(int timeout_ms);
    ucp_worker_h handle() const;
};
```

#### Layer 2: Transport Abstraction Layer

```cpp
class TransportBackend {
public:
    virtual ~TransportBackend() = default;
    virtual Status init(const Config& cfg) = 0;
    virtual Status shutdown() = 0;

    virtual Status send_tag(Endpoint& ep, const void* buf, size_t len,
                            Tag tag, Request& req) = 0;
    virtual Status recv_tag(Worker& w, void* buf, size_t len,
                            Tag tag, Tag mask, Request& req) = 0;
    virtual Status put(Endpoint& ep, const void* local, size_t len,
                       uint64_t remote_addr, const RemoteKey& rkey,
                       Request& req) = 0;
    virtual Status get(Endpoint& ep, void* local, size_t len,
                       uint64_t remote_addr, const RemoteKey& rkey,
                       Request& req) = 0;
    virtual std::unique_ptr<Endpoint> connect(Worker& w,
                                               const std::string& addr) = 0;
    virtual Status listen(Worker& w, const std::string& addr,
                          ConnectionCallback cb) = 0;
};

class ProtocolSelector {
public:
    enum class Protocol { kEager, kRendezvous };
    Protocol select(size_t msg_size, MemoryType mem_type) const;
    void set_threshold(size_t threshold);  // 默认 8KB
};
```

#### Layer 3: Memory Management Layer

- `MemoryRegion::register_mem()` / `MemoryRegion::allocate()` -- 注册/分配
- `MemoryPool::create()` / `allocate()` / `deallocate()` -- 池化分配
- `RegistrationCache::get_or_register()` / `invalidate()` -- LRU 缓存

#### Layer 4: Plugin Layer

```cpp
class Plugin {
public:
    virtual ~Plugin() = default;
    virtual const std::string& name() const = 0;
    virtual Status init(const Context::Ptr& ctx) = 0;
    virtual Status shutdown() = 0;
};

class CollectivePlugin : public Plugin {
public:
    virtual Status all_reduce(const void* send, void* recv, size_t count,
                              DataType dtype, ReduceOp op,
                              const Communicator& comm) = 0;
    virtual Status all_gather(const void* send, void* recv, size_t count,
                              const Communicator& comm) = 0;
    virtual Status broadcast(void* buf, size_t count, int root,
                             const Communicator& comm) = 0;
    virtual Status all_to_all(const void* send, void* recv, size_t count,
                              const Communicator& comm) = 0;
};

class PluginRegistry {
public:
    static PluginRegistry& instance();
    void register_plugin(std::unique_ptr<Plugin> plugin);
    Status load_plugin(const std::string& path);
    Plugin* find(const std::string& name);
};

#define P2P_REGISTER_PLUGIN(PluginClass) \
    static struct PluginClass##_Registrar { \
        PluginClass##_Registrar() { \
            PluginRegistry::instance().register_plugin( \
                std::make_unique<PluginClass>()); \
        } \
    } g_##PluginClass##_registrar;
```

### 2.3 Plugin 架构设计 (NCCL/HCCL 扩展)

```
+-------------------------------------------+
|           Application Code                |
+-------------------------------------------+
|        CollectivePlugin API               |
|  all_reduce / all_gather / broadcast      |
+-------------------------------------------+
       |                    |
  +----------+       +----------+
  |  NCCL    |       |  HCCL    |       ... (可扩展)
  |  Plugin  |       |  Plugin  |
  +----------+       +----------+
       |                    |
  +----------+       +----------+
  | libnccl  |       | libhccl  |
  +----------+       +----------+
```

**加载方式：**
1. **静态链接**：编译时通过 CMake `option(P2P_ENABLE_NCCL ...)` 控制
2. **动态加载**：运行时通过 `PluginRegistry::load_plugin("libp2p_nccl.so")` 或环境变量 `P2P_PLUGINS=nccl,hccl`
3. **自动发现**：扫描 `${P2P_PLUGIN_DIR}/*.so`，调用约定的 `p2p_plugin_create()` 入口函数

---

## 三、关键技术决策

### 3.1 消息传输策略：Eager vs Rendezvous

```
            Eager                          Rendezvous
  Sender ---------> Receiver     Sender <-------> Receiver
  (直接拷贝到网络)                 (协商后零拷贝 RDMA)

  适合：小消息 (<= 阈值)           适合：大消息 (> 阈值)
  优点：低延迟                     优点：零拷贝，高带宽
  缺点：需要接收端预分配缓冲区       缺点：额外握手开销
```

**阈值策略：**

| 消息大小 | 协议 | 原因 |
|---------|------|------|
| <= 8KB | Eager short/bcopy | 消息直接内联到控制消息或 bounce buffer 拷贝 |
| 8KB - 256KB | Eager zcopy | 零拷贝 eager（已注册内存） |
| > 256KB | Rendezvous | 三步握手 + RDMA read/write |

GPU 内存的阈值应更低（如 4KB），因为 GPU 内存的 bounce buffer 拷贝代价极高。

### 3.2 线程模型

```
方案 A: 每线程一个 Worker (推荐)
+--------+  +--------+  +--------+  +--------+
|Thread 0|  |Thread 1|  |Thread 2|  |Thread 3|
|Worker 0|  |Worker 1|  |Worker 2|  |Worker 3|
+--------+  +--------+  +--------+  +--------+
  SINGLE      SINGLE      SINGLE      SINGLE
```

| 方案 | 延迟 | 吞吐 | 可扩展性 | 编程复杂度 | 适用场景 |
|------|------|------|---------|-----------|---------|
| A: 每线程一个 Worker | 最低 | 最高 | 线性扩展 | 低 | 核数已知、线程模型固定 |
| B: 单 Worker 事件驱动 | 中 | 受限于单核 | 差 | 中 | 低并发、简化调试 |
| C: Worker Pool | 低 | 高 | 灵活 | 高 | 线程数远超核数 |

**推荐方案 A (默认) + 方案 C (可选)**

### 3.3 内存管理

```
+------------------------------------------------------------------+
|                    RegistrationCache (LRU)                        |
|  key: (addr, len, mem_type) -> value: MemoryRegion::Ptr          |
+------------------------------------------------------------------+
|                    MemoryPool (Slab Allocator)                    |
|  预分配大块已注册内存, 按 slab 分配                                 |
|  Size Classes: 1KB, 4KB, 16KB, 64KB, 256KB, 1MB, 4MB...1GB      |
+------------------------------------------------------------------+
|                    UCX Memory Registration                        |
|  ucp_mem_map() -> ucp_mem_h  (pin + register with NIC)           |
+------------------------------------------------------------------+
```

### 3.4 连接管理

- 连接按需建立 (lazy connect)，`ucp_ep_create()` 本身非阻塞
- 每个 (Worker, remote_addr) 对维护一个 Endpoint
- 空闲连接超时回收（默认 5 分钟）
- 连接异常时自动重连（指数退避）

---

## 四、软件选型

| 类别 | 选择 | 理由 |
|------|------|------|
| 构建系统 | **CMake 3.20+** | UCX/CUDA/NCCL 生态最好 |
| C++ 标准 | **C++20** | std::span, concepts |
| Python 绑定 | **nanobind** | 编译快 3x、运行快 3-10x、体积小 3-5x vs pybind11 |
| 序列化 | **核心不依赖；控制消息可选 flatbuffers** | 传输库操作 raw buffer |
| 日志 | **spdlog** | 异步、高性能、header-only |
| 测试 | **Google Test + Google Benchmark** | 业界标准 |

---

## 五、推荐目录结构

```
p2p/
├── CMakeLists.txt
├── cmake/
│   ├── FindUCX.cmake
│   ├── FindNCCL.cmake
│   └── FindHCCL.cmake
├── include/p2p/
│   ├── common.h              # 错误码、Tag、MemoryType
│   ├── config.h              # Config + Context
│   ├── memory.h              # MemoryRegion/Pool/Cache
│   ├── worker.h              # Worker
│   ├── endpoint.h            # Endpoint + Future<T>
│   ├── transport.h           # TransportBackend 接口
│   └── plugin.h              # Plugin + PluginRegistry
├── src/
│   ├── core/
│   ├── transport/ucx/
│   ├── memory/
│   └── plugin/nccl/ + hccl/
├── python/
│   ├── p2p_ext.cpp           # nanobind 绑定
│   └── p2p/
├── tests/unit/ + integration/ + benchmark/
└── examples/
```

---

**参考资料：**
- [UCX Design - OpenUCX API](https://openucx.github.io/ucx/api/)
- [OpenUCX FAQ](https://openucx.readthedocs.io/en/master/faq.html)
- [UCX Rendezvous Protocol Wiki](https://github.com/openucx/ucx/wiki/Rendezvous-Protocol)
- [UCX NVIDIA GPU Support Wiki](https://github.com/openucx/ucx/wiki/NVIDIA-GPU-Support)
- [UCX Memory Management Wiki](https://github.com/openucx/ucx/wiki/UCX-Memory-management)
- [UCX Thread Safety Research](https://link.springer.com/article/10.1007/s11227-024-06201-x)
- [nanobind Benchmarks](https://nanobind.readthedocs.io/en/latest/benchmark.html)
