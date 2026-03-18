# AXON 高性能传输库 — 接口设计与实现方案

> 角色: 开发工程师 (Dev)
> 日期: 2026-03-04

---

## 一、C++ API 设计

### 1.1 文件结构与层次

```
include/axon/
  common.h         -- 错误码、Tag、MemoryType、前向声明
  config.h         -- Config (Builder模式) + Context (顶层句柄)
  memory.h         -- MemoryRegion / MemoryPool / RegistrationCache
  future.h         -- Request + Future<T> + CompletionCallback
  worker.h         -- Worker (progress引擎) + Listener
  endpoint.h       -- Endpoint (双边/单边/流式传输)
  plugin/plugin.h  -- CollectivePlugin + PluginRegistry
  axon.h            -- 便捷聚合头文件
```

### 1.2 核心设计原则

**错误处理** (`common.h`):
- 采用 `Status` 类 + `ErrorCode` 枚举，与 `std::error_code` 互操作
- 同步路径返回 Status，异步路径通过 Future 传播错误
- `throw_if_error()` 方法支持异常模式

**Context/Config** (`config.h`):
- Config 使用 Builder 模式，支持链式调用 + `from_env()` 环境变量覆盖
- Context 是 `shared_ptr` 语义，生命周期管理所有子资源
- 支持查询硬件能力：RMA、硬件 tag matching、内存类型

**异步模型** (`future.h`):
- `Request` 是低级句柄（对应 UCX ucs_status_ptr_t）
- `Future<T>` 是类型安全的异步结果，支持：
  - `ready()` 非阻塞轮询
  - `get()` 阻塞等待
  - `then()` 链式组合
  - `on_complete()` 回调模式
- `wait_all()` / `wait_any()` 批量等待

**传输接口** (`endpoint.h`):
- **双边 (tag-matched)**：`tag_send()` / `tag_recv()`，支持 raw buffer 和 MemoryRegion 两种重载
- **单边 RDMA**：`put()` / `get()` / `flush()`，基于 RemoteKey
- **原子操作**：`atomic_fadd()` / `atomic_cswap()`
- **流式**：`stream_send()` / `stream_recv()`（无 tag，有序字节流）

**内存管理** (`memory.h`):
- `MemoryRegion`：注册或分配+注册一体化，暴露 `remote_key()` 和 `as_span<T>()`
- `MemoryPool`：预注册的 slab 分配器，消除热路径上的注册开销
- `RegistrationCache`：LRU + 区间树，自动缓存重复注册

**Tag 设计** (`common.h`):
- 64-bit tag：高 32 位是 context/communicator ID，低 32 位是用户 tag
- `make_tag()` / `tag_context()` / `tag_user()` 辅助函数
- `kTagAny` / `kTagMaskAll` / `kTagMaskUser` 用于灵活匹配

### 1.3 代码文件清单

所有头文件已生成并保存：

| 文件 | 行数 | 内容 |
|------|------|------|
| `include/axon/common.h` | 146 | ErrorCode, Status, Tag, MemoryType |
| `include/axon/config.h` | 149 | Config::Builder + Context |
| `include/axon/memory.h` | 182 | MemoryRegion/Pool/Cache |
| `include/axon/future.h` | 152 | Request + Future<T> |
| `include/axon/worker.h` | 144 | Worker + Listener |
| `include/axon/endpoint.h` | 162 | Endpoint: tag/RMA/stream/atomic |
| `include/axon/plugin/plugin.h` | 254 | CollectivePlugin + Registry |
| `include/axon/axon.h` | 12 | 聚合头文件 |
| `examples/cpp_usage.cpp` | 222 | 6个C++使用示例 |

---

## 二、Python API 设计

### 2.1 与 C++ 的对应关系

| C++ | Python | 说明 |
|-----|--------|------|
| `Config::Builder` | `Config(**kwargs)` | Pythonic 关键字参数替代 Builder |
| `Context::create(cfg)` | `Context(transport="ucx", ...)` | 支持直传参数或 config= |
| `Worker::create(ctx)` | `ctx.create_worker()` | 方法调用而非静态工厂 |
| `Future<T>` | `await` / `Future` | 实现 `__await__` 协议 |
| `MemoryRegion` | `MemoryRegion` + buffer protocol | 支持 `np.frombuffer(region)` |
| `CompletionCallback` | asyncio Future resolve | 通过 `call_soon_threadsafe` 桥接 |

### 2.2 asyncio 集成方案

核心思路：

1. **Worker.attach_to_event_loop()**：将 UCX worker 的 `event_fd` 注册到 asyncio 的 `add_reader()`
2. 当 UCX 有事件就绪时，event loop 自动调用 `progress()`
3. C++ `Future<T>` 的 `on_complete` 回调通过 `loop.call_soon_threadsafe()` 解析 asyncio.Future
4. 所有阻塞 C++ 调用使用 `py::gil_scoped_release` 释放 GIL

### 2.3 零拷贝支持

- `tag_send()` / `tag_recv()` 接受任何实现 buffer protocol 的对象（numpy, cupy, memoryview, bytearray）
- 通过 `py::buffer::request()` 提取原始指针和长度，然后释放 GIL 进入 C++
- `MemoryRegion` 自身也实现 `__buffer__`，支持 `np.frombuffer(region)` 零拷贝视图
- cupy 数组的设备指针通过 `__cuda_array_interface__` 传递

### 2.4 代码文件清单

| 文件 | 行数 | 内容 |
|------|------|------|
| `python/axon/_core.pyi` | 496 | 完整类型 stub |
| `python/axon/__init__.py` | 68 | 包入口 |
| `src/python/bindings.cpp` | 367 | nanobind 绑定实现 |
| `examples/python_usage.py` | 263 | 8个Python使用示例 |

---

## 三、NCCL/HCCL Plugin 接口设计

### 3.1 Plugin 需要实现的接口

定义在 `include/axon/plugin/plugin.h` 中的 `CollectivePlugin` 抽象类：

- **元数据**：`name()`, `version()`, `supported_memory_types()`
- **生命周期**：`init(ctx)`, `shutdown()`
- **通信子管理**：`generate_unique_id()`, `create_communicator()`, `destroy_communicator()`
- **集合通信**：`allreduce()`, `broadcast()`, `allgather()`, `reduce_scatter()`, `alltoall()`
- **点对点**：`send()`, `recv()`
- **分组**：`group_start()`, `group_end()`
- **传输集成**（可选）：`register_transport()` 允许 plugin 使用 AXON 传输层做 fallback

### 3.2 注册和发现机制

```
1. 每个 plugin .so 导出 C 工厂函数：
   AXON_PLUGIN_EXPORT CollectivePlugin* axon_plugin_create();

2. PluginRegistry 发现方式：
   - 手动注册：  registry.register_plugin(std::make_unique<MyPlugin>())
   - 指定路径：  registry.load_plugin("/path/to/libaxon_plugin_nccl.so")
   - 目录扫描：  registry.discover("/usr/lib/axon/plugins/")

3. 查找：       auto* nccl = registry.find("nccl");
```

### 3.3 参考实现

NCCL Plugin 骨架代码：`src/plugin/nccl_plugin.cpp` (229行)

---

## 四、关键实现挑战

### 4.1 UCX Worker 线程安全

**问题**：`ucp_worker_h` 不是线程安全的。

**方案**：每线程一个 Worker + 无锁 MPSC 队列跨线程提交：
- 外部线程将 WorkItem 入队到 `MPSCQueue`
- Worker 线程在 `progress()` 中 drain 队列并执行
- 通过 `eventfd` 唤醒 `epoll_wait` 中阻塞的 Worker

### 4.2 大消息零拷贝

**方案**：UCX rendezvous 协议 + 预注册
- 预注册的 `MemoryRegion` 直接走 RDMA read，0 次拷贝
- 未注册 buffer 先查 `RegistrationCache`，miss 时 pin + cache
- 阈值：< 8KB eager inline，8-256KB eager zcopy，> 256KB rendezvous

### 4.3 Python GIL

**方案**：多层 GIL 释放策略
- 所有 C++ blocking 调用使用 `py::gil_scoped_release`
- asyncio 模式：通过 `event_fd` + `add_reader` 驱动 progress，极少 GIL 争用
- buffer protocol 提取指针时持 GIL，提取后立即释放
- 备选方案：C++ 专用 progress 线程完全不接触 GIL

GIL 开销测量：acquire/release ~50-100ns vs RDMA send 4KB ~2-5us，对 >= 4KB 消息影响 < 5%。

### 4.4 Registration Cache

**方案**：LRU + 区间树
- 区间树 O(log n) 范围查找，支持部分重叠时的合并注册
- LRU 淘汰 + 引用计数保护在途操作
- `invalidate()` 接口供用户在 free 前调用
- 典型 ML 训练场景命中率 > 95%

详细技术方案记录在 `src/transport/ucx_impl_notes.h` (264行)
