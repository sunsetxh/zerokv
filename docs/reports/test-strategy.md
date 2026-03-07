# P2P 高性能传输库 — 测试策略文档

> 版本: 1.0
> 日期: 2026-03-04
> 角色: 测试工程师

---

## 目录

1. [性能基线与目标](#1-性能基线与目标)
2. [测试策略设计](#2-测试策略设计)
3. [故障测试场景](#3-故障测试场景)
4. [CI/CD 建议](#4-cicd-建议)
5. [质量门禁](#5-质量门禁)
6. [参考资料](#6-参考资料)

---

## 1. 性能基线与目标

### 1.1 UCX 原生性能基线

UCX 作为底层通信框架，其原生性能是本项目的性能上限参考。以下数据来自 ucx_perftest、OSU Micro-Benchmarks 及公开论文。

| 消息大小 | 传输类型 | 延迟 (单程) | 吞吐/带宽 | 说明 |
|----------|----------|------------|-----------|------|
| 8 B | IB Verbs (HDR) | ~1.0 us | - | 小消息延迟主导 |
| 4 KB | IB Verbs (HDR) | ~2.5 us | ~1.5 GB/s | eager 协议 |
| 64 KB | IB Verbs (HDR) | ~5 us | ~10 GB/s | rendezvous 切换点附近 |
| 1 MB | IB Verbs (HDR) | ~50 us | ~23 GB/s | 接近线速 |
| 4 MB+ | IB Verbs (HDR) | - | ~24 GB/s | 线速 (200Gbps) |
| 8 B | 共享内存 (sm) | ~0.12 us | - | 进程间最低延迟 |
| 1 MB | 共享内存 (sm) | ~15 us | ~12 GB/s | 受内存带宽限制 |
| 8 B | TCP | ~10 us | - | 内核协议栈开销 |
| 1 MB | TCP | ~200 us | ~3 GB/s | 受 CPU 和协议栈限制 |

> 注: 以上数据为典型参考值，实际性能取决于硬件配置、驱动版本、BIOS 设置等。

### 1.2 NCCL 在不同 GPU 拓扑下的性能

| 拓扑 | 算法带宽 (AllReduce) | Bus 带宽 | 说明 |
|------|---------------------|----------|------|
| PCIe (无 NVLink) | ~5 GB/s | ~10 GB/s | 受 PCIe 带宽限制 |
| PCIe 直连 GPU-GPU | ~12 GB/s | ~12 GB/s | 跳过 CPU |
| NVLink (Pascal, 4路) | ~40 GB/s | ~62 GB/s | 4x NVLink 1.0 |
| NVLink (Volta, 6路) | ~80 GB/s | ~132 GB/s | 6x NVLink 2.0 |
| NVLink (A100, 12路) | ~250 GB/s | ~600 GB/s | NVSwitch 全互联 |
| 跨节点 IB HDR | ~23 GB/s | ~23 GB/s | 200Gbps 线速 |
| 跨节点 IB NDR | ~48 GB/s | ~48 GB/s | 400Gbps 线速 |

### 1.3 InfiniBand 理论与实测带宽

| 规格 | 理论带宽 | 单向实测带宽 | 延迟 (ib_send_lat) |
|------|---------|------------|-------------------|
| HDR (200 Gbps) | 25 GB/s | ~23.5 GB/s (94%) | ~0.6 us (ConnectX-6) |
| NDR (400 Gbps) | 50 GB/s | ~48 GB/s (96%) | ~0.5 us (ConnectX-7) |
| HDR100 (100 Gbps) | 12.5 GB/s | ~11.8 GB/s | ~0.7 us |

> 实测值通常达到理论值的 93%–97%，损耗来自编码开销 (64b/66b) 和协议头。

### 1.4 RoCE v2 典型性能

| 指标 | RoCE v2 | 原生 InfiniBand | 说明 |
|------|---------|----------------|------|
| 小消息延迟 | 2–4 us | ~1 us | RoCE 额外 UDP/IP 封装 |
| 大消息带宽 | ~23 GB/s (200G) | ~23.5 GB/s | 几乎持平 |
| 拥塞下吞吐 | 有退化 | 基本稳定 | IB 有信用流控,RoCE 依赖 PFC/ECN |
| NCCL Bus BW | ~195 GB/s (8卡) | ~195 GB/s | Ring AllReduce 几乎一致 |

> RoCE v2 在无拥塞场景下带宽与 IB 持平，延迟差距约 2–3 us；拥塞场景下需依赖 DCQCN 等机制。

### 1.5 共享内存传输性能参考

| 实现方式 | 消息大小 | 延迟 | 吞吐 |
|---------|---------|------|------|
| POSIX shm + 无锁环形缓冲 | 64 B | ~0.12 us | ~780 万 msg/s |
| POSIX shm + mutex | 4 KB | ~1.4 us | ~51 万 msg/s |
| UCX sm 传输 | 8 B | ~0.12 us | - |
| UCX sm 传输 | 1 MB | ~15 us | ~12 GB/s |
| mmap 大块拷贝 | 1 GB | ~5 ms | ~50 GB/s (DDR5) |

### 1.6 本项目性能目标

基于以上基线数据，定义本项目各场景的性能目标：

| 场景 | 指标 | 目标值 | 与原生 UCX 对比 overhead |
|------|------|--------|------------------------|
| 小消息延迟 (<=4KB, RDMA) | 单程延迟 | <=2.0 us | <=100% (即不超过 2x UCX 原生) |
| 小消息延迟 (<=4KB, shm) | 单程延迟 | <=0.5 us | <=300% |
| 大消息吞吐 (>=1MB, RDMA) | 带宽 | >=90% 线速 | <=5% 带宽损失 |
| 大消息吞吐 (>=1MB, shm) | 带宽 | >=10 GB/s | <=20% 带宽损失 |
| 连接建立 | 首次通信延迟 | <=10 ms | 一次性成本,可接受 |
| Python 接口 overhead | 相比 C++ 接口 | <=10% 额外延迟 | 大消息摊薄后可忽略 |
| GPU 内存传输 (GPUDirect) | 带宽 | >=85% 线速 | <=10% 带宽损失 |
| 并发 (64路并行传输) | 聚合吞吐 | >=80% 线速 | 调度/锁竞争导致的损失 |

**核心原则**: 对于 >= 64KB 的消息，P2P 库相比原生 UCX 的 overhead 不超过 5%；对于 < 4KB 的小消息，overhead 不超过 2us (绝对值)。

---

## 2. 测试策略设计

### 2.1 单元测试

#### 2.1.1 核心模块测试覆盖

| 模块 | 测试重点 | 用例数(估计) |
|------|---------|-------------|
| Endpoint 管理 | 创建/销毁、地址解析、状态机 | 20–30 |
| 内存注册 (Memory Registration) | 注册/注销、pin/unpin、GPU 内存 | 15–20 |
| 传输层抽象 | 协议选择、消息分片/重组、流控 | 25–35 |
| 连接管理 | 握手、超时、重连、并发连接 | 20–25 |
| 缓冲区管理 | 分配/回收、池化、对齐 | 15–20 |
| 序列化/元数据 | header 编解码、版本兼容 | 10–15 |
| 配置管理 | 参数解析、默认值、校验 | 10–15 |
| Python 绑定 | 接口映射、GIL 释放、引用计数 | 15–20 |

#### 2.1.2 Mock UCX 层方案

```
┌─────────────────────────────────┐
│        P2P Library Code         │
│  (Endpoint, Transport, Buffer)  │
├─────────────────────────────────┤
│     UCX Abstraction Layer       │  <-- 接口层 (纯虚类/函数指针表)
├────────────┬────────────────────┤
│  Real UCX  │   Mock UCX Impl   │
│  Backend   │   (gtest/gmock)   │
└────────────┴────────────────────┘
```

**设计要点:**

1. **接口抽象层**: 定义 `IUcxContext`, `IUcxWorker`, `IUcxEndpoint`, `IUcxMemory` 等纯虚接口，生产代码通过接口调用 UCX，测试代码注入 Mock 实现。

2. **GMock 实现示例**:
```cpp
class MockUcxWorker : public IUcxWorker {
public:
    MOCK_METHOD(Status, progress, (), (override));
    MOCK_METHOD(Status, sendNb, (const Endpoint&, const void*, size_t, SendCallback), (override));
    MOCK_METHOD(Status, recvNb, (void*, size_t, RecvCallback), (override));
    MOCK_METHOD(Status, flush, (), (override));
};

// 测试用例示例
TEST(TransportTest, SendSmallMessage_UsesEagerProtocol) {
    MockUcxWorker worker;
    EXPECT_CALL(worker, sendNb(_, _, Lt(8192), _))
        .WillOnce(DoAll(InvokeArgument<3>(Status::OK), Return(Status::OK)));

    Transport transport(&worker);
    auto status = transport.send(endpoint, data, 1024);
    EXPECT_TRUE(status.ok());
}
```

3. **状态注入**: Mock 可模拟各种 UCX 返回状态 (`UCS_OK`, `UCS_ERR_NO_RESOURCE`, `UCS_INPROGRESS` 等)，无需真实硬件。

4. **回调模拟**: 通过 GMock 的 `InvokeArgument` 或自定义 Action 模拟异步完成回调，测试异步逻辑的正确性。

#### 2.1.3 内存管理测试

```cpp
// 使用 ASAN (AddressSanitizer) 编译选项检测内存问题
// CMake: -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"

TEST(BufferPoolTest, AllocateAndFree_NoLeak) {
    BufferPool pool(/*capacity=*/100, /*buffer_size=*/4096);
    std::vector<Buffer*> buffers;
    for (int i = 0; i < 100; i++) {
        buffers.push_back(pool.allocate());
        ASSERT_NE(buffers.back(), nullptr);
    }
    // 池已满
    EXPECT_EQ(pool.allocate(), nullptr);

    for (auto* buf : buffers) {
        pool.free(buf);
    }
    EXPECT_EQ(pool.available(), 100);
}
```

### 2.2 集成测试

#### 2.2.1 多进程测试方案

```
┌──────────────┐     ┌──────────────┐
│  Process A   │────>│  Process B   │   同节点 (shm/tcp)
│  (sender)    │<────│  (receiver)  │
└──────────────┘     └──────────────┘

┌──────────────┐     ┌──────────────┐
│   Node 1     │────>│   Node 2     │   跨节点 (RDMA/tcp)
│  Process A   │<────│  Process B   │
└──────────────┘     └──────────────┘
```

**测试编排框架:**

```python
# pytest + multiprocessing 方案 (同节点)
import pytest
import multiprocessing as mp
from p2p import Endpoint, Config

def sender_proc(addr_queue, data_size):
    ep = Endpoint(Config(transport="shm"))
    addr_queue.put(ep.address)
    peer_addr = addr_queue.get()
    ep.connect(peer_addr)

    data = bytes(data_size)
    ep.send(data)
    ack = ep.recv()
    assert ack == b"OK"

def receiver_proc(addr_queue, data_size):
    ep = Endpoint(Config(transport="shm"))
    addr_queue.put(ep.address)
    peer_addr = addr_queue.get()
    ep.connect(peer_addr)

    data = ep.recv()
    assert len(data) == data_size
    ep.send(b"OK")

@pytest.mark.parametrize("data_size", [1024, 65536, 1048576, 1073741824])
def test_send_recv(data_size):
    q1, q2 = mp.Queue(), mp.Queue()
    # 使用双队列交换地址，避免死锁
    p1 = mp.Process(target=sender_proc, args=((q1, q2), data_size))
    p2 = mp.Process(target=receiver_proc, args=((q2, q1), data_size))
    p1.start(); p2.start()
    p1.join(timeout=60); p2.join(timeout=60)
    assert p1.exitcode == 0
    assert p2.exitcode == 0
```

**多节点测试方案:**

```bash
#!/bin/bash
# 使用 MPI 启动跨节点测试
# mpirun -np 2 --hostfile hostfile ./p2p_integration_test

# 或使用 SSH + 自定义脚本
NODE_A="10.0.0.1"
NODE_B="10.0.0.2"

ssh $NODE_A "./p2p_test_server --port 12345 --transport rdma" &
SERVER_PID=$!
sleep 2
ssh $NODE_B "./p2p_test_client --server $NODE_A:12345 --transport rdma --size 1M"
CLIENT_EXIT=$?

ssh $NODE_A "kill $SERVER_PID" 2>/dev/null
exit $CLIENT_EXIT
```

#### 2.2.2 端到端测试场景

| 场景 ID | 描述 | 验证点 |
|---------|------|--------|
| E2E-001 | 单对单基本收发 | 数据完整性 (CRC32/SHA256) |
| E2E-002 | 大消息分片传输 | 分片重组正确性 |
| E2E-003 | 双向同时传输 | 无死锁，数据正确 |
| E2E-004 | 多对一扇入 | 并发安全，无数据混淆 |
| E2E-005 | 一对多扇出 | 广播语义正确性 |
| E2E-006 | GPU 内存直传 | GPUDirect RDMA 路径验证 |
| E2E-007 | 混合内存类型 | host->GPU, GPU->host 传输 |
| E2E-008 | 传输协议回退 | RDMA 不可用时回退到 TCP |
| E2E-009 | Python/C++ 互操作 | Python 发 C++ 收，反之 |
| E2E-010 | HCCL/NCCL 扩展接口 | 集合通信操作正确性 |

### 2.3 性能测试

#### 2.3.1 延迟测试 (Ping-Pong)

**方法论:**

```
  Sender                  Receiver
    │                        │
    ├── record T1 ──────────>│
    │       send(msg)        │
    │                        ├── recv(msg)
    │                        ├── send(ack)
    │<───────────────────────┤
    ├── record T2            │
    │                        │
    RTT = T2 - T1
    One-way latency ≈ RTT / 2
```

**实现要点:**

1. **预热 (Warmup)**: 前 1000 次迭代丢弃，消除 JIT、缓存冷启动、连接建立等影响。
2. **迭代次数**: 小消息 >= 100,000 次；大消息 >= 1,000 次。
3. **时钟选择**: 使用 `clock_gettime(CLOCK_MONOTONIC)` 或 `rdtsc` (需校准)。
4. **统计方法**: 报告 min / p50 / p95 / p99 / max，同时报告标准差。
5. **CPU 绑定**: 使用 `taskset` 或 `numactl` 固定 CPU，避免调度抖动。
6. **干扰排除**: 关闭 irqbalance，设置 CPU governor 为 performance。

```cpp
struct LatencyResult {
    double min_us, max_us;
    double p50_us, p95_us, p99_us;
    double mean_us, stddev_us;
    size_t iterations;
    size_t message_size;
    std::string transport;
};

LatencyResult run_pingpong(Endpoint& ep, size_t msg_size,
                           size_t warmup = 1000, size_t iterations = 100000) {
    std::vector<double> latencies;
    latencies.reserve(iterations);

    std::vector<uint8_t> buf(msg_size, 0xAB);

    // Warmup
    for (size_t i = 0; i < warmup; i++) {
        ep.send(buf.data(), msg_size);
        ep.recv(buf.data(), msg_size);
    }

    // Measurement
    for (size_t i = 0; i < iterations; i++) {
        auto t1 = steady_clock::now();
        ep.send(buf.data(), msg_size);
        ep.recv(buf.data(), msg_size);
        auto t2 = steady_clock::now();
        latencies.push_back(duration_cast<nanoseconds>(t2 - t1).count() / 2000.0);
    }

    return compute_stats(latencies, msg_size);
}
```

#### 2.3.2 吞吐测试 (Streaming)

**方法论:**

```
  Sender                          Receiver
    │                                │
    ├─ send(msg[0]) ────────────────>│
    ├─ send(msg[1]) ────────────────>│  流水线发送
    ├─ send(msg[2]) ────────────────>│  无需等待 ACK
    │  ...                           │
    ├─ send(msg[N-1]) ──────────────>│
    ├─ send(FIN) ───────────────────>│
    │<── recv(total_bytes_received) ──┤
    │                                │
    Throughput = total_bytes / elapsed_time
```

**实现要点:**

1. **窗口大小**: 使用滑动窗口控制在途消息数量 (典型值 16–64)。
2. **总数据量**: 确保传输总量 >= 1 GB 以摊薄启动/结束开销。
3. **异步发送**: 使用非阻塞发送 + 完成队列，最大化管道利用率。
4. **报告方式**: GB/s (带宽)，Mmsg/s (消息速率)。

#### 2.3.3 测试矩阵

**消息大小:**
```
1KB, 4KB, 16KB, 64KB, 256KB, 1MB, 4MB, 16MB, 64MB, 256MB, 1GB
```

**传输协议:**
```
shm (共享内存), tcp, rc_verbs (IB RC), rc_mlx5 (优化路径), ud_verbs, roce
```

**内存类型:**
```
host (malloc), host_pinned (mlock), cuda (cudaMalloc), cuda_managed (cudaMallocManaged)
```

**并发度:**
```
1, 2, 4, 8, 16, 32, 64 并行流
```

**完整矩阵 (关键组合):**

| 消息大小 | 传输协议 | 内存类型 | 并发度 | 优先级 |
|---------|---------|---------|--------|--------|
| 1KB–4KB | shm | host | 1 | P0 |
| 1KB–4KB | rc_verbs | host | 1 | P0 |
| 64KB–1MB | rc_verbs | host | 1,8,64 | P0 |
| 1MB–1GB | rc_verbs | host | 1,8,64 | P0 |
| 1MB–1GB | rc_verbs | cuda | 1,8 | P0 |
| 1KB–1GB | tcp | host | 1,8 | P1 |
| 1MB–1GB | shm | host | 1,8,64 | P1 |
| 64KB–1GB | roce | host | 1,8,64 | P1 |
| 1KB–1GB | rc_verbs | cuda_managed | 1 | P2 |
| 1KB–4KB | rc_verbs | host_pinned | 1 | P2 |

> P0 = 每次提交必测; P1 = 每日回归; P2 = 发布前全量测试

**预期结果存储格式 (JSON):**
```json
{
    "test_id": "perf-lat-rc_verbs-host-4KB-1",
    "timestamp": "2026-03-04T10:00:00Z",
    "config": {
        "message_size": 4096,
        "transport": "rc_verbs",
        "memory_type": "host",
        "concurrency": 1
    },
    "results": {
        "latency_us": {"p50": 2.1, "p95": 2.8, "p99": 3.5, "min": 1.9, "max": 12.0},
        "bandwidth_gbps": null,
        "iterations": 100000
    },
    "environment": {
        "ucx_version": "1.17.0",
        "os": "Ubuntu 22.04",
        "kernel": "5.15.0",
        "nic": "ConnectX-6",
        "cpu": "EPYC 7763"
    }
}
```

### 2.4 压力测试

#### 2.4.1 长时间运行测试

| 测试 | 持续时间 | 监控指标 | 通过标准 |
|------|---------|---------|---------|
| 持续传输 (小消息) | 24 小时 | 延迟 p99，RSS 内存 | 延迟无趋势性增长;内存增长 < 1% |
| 持续传输 (大消息) | 12 小时 | 吞吐, CPU 利用率 | 吞吐波动 < 5% |
| 连接反复建断 | 8 小时 | FD 数, 内存, 连接成功率 | 无 FD 泄漏; 成功率 100% |
| 混合负载 | 24 小时 | 以上所有 | 综合稳定 |

#### 2.4.2 大量连接测试

```
测试规模:
- 单进程维持 1,000 个并发连接
- 每秒建立/断开 100 个连接 (churn test)
- 10,000 个连接的内存占用量测
```

#### 2.4.3 内存泄漏检测

**工具链:**

1. **编译期**: `-fsanitize=address,leak` (ASAN/LSAN)
2. **运行期**: Valgrind (memcheck, massif)
3. **生产级**: `jemalloc` 内置 profiling (`MALLOC_CONF="prof:true,prof_leak:true"`)
4. **自定义**: RSS 监控脚本

```bash
#!/bin/bash
# 运行时 RSS 监控
PID=$1
INTERVAL=5
LOG="rss_monitor_${PID}.csv"
echo "timestamp,rss_kb,vms_kb" > $LOG

while kill -0 $PID 2>/dev/null; do
    RSS=$(awk '/VmRSS/ {print $2}' /proc/$PID/status)
    VMS=$(awk '/VmSize/ {print $2}' /proc/$PID/status)
    echo "$(date +%s),$RSS,$VMS" >> $LOG
    sleep $INTERVAL
done
```

**UCX 内存注册泄漏检测:**

```cpp
TEST(MemoryRegistration, NoLeakAfterDeregister) {
    auto initial_registrations = ucx_context.getRegistrationCount();

    for (int i = 0; i < 10000; i++) {
        auto mem = ucx_context.registerMemory(buffer, size);
        ucx_context.deregisterMemory(mem);
    }

    EXPECT_EQ(ucx_context.getRegistrationCount(), initial_registrations);
}
```

### 2.5 兼容性测试

#### 2.5.1 测试矩阵

| 维度 | 覆盖范围 |
|------|---------|
| **OS** | Ubuntu 22.04, Ubuntu 24.04, CentOS 8, RHEL 9, Rocky 9 |
| **内核** | 5.15 LTS, 6.1 LTS, 6.6 LTS |
| **UCX 版本** | 1.14.x, 1.15.x, 1.16.x, 1.17.x (latest) |
| **CUDA** | 11.8, 12.0, 12.4, 12.6 |
| **GPU 驱动** | 535.x, 545.x, 550.x |
| **Python** | 3.9, 3.10, 3.11, 3.12 |
| **编译器** | GCC 11, GCC 12, GCC 13, Clang 15, Clang 17 |
| **NIC** | ConnectX-5, ConnectX-6, ConnectX-7 |
| **OFED** | MLNX_OFED 5.8, 5.9, 23.10, 24.01 |

#### 2.5.2 最小覆盖策略 (Pairwise)

全量组合不可行 (数千种)，采用 pairwise 组合测试，用约 20–30 组配置覆盖所有两两组合。

---

## 3. 故障测试场景

### 3.1 连接断开

| 场景 ID | 描述 | 注入方法 | 预期行为 |
|---------|------|---------|---------|
| FAULT-001 | 传输中途网络断开 | `iptables -A INPUT -j DROP` 或拔线 | 检测超时, 回调错误码, 不崩溃 |
| FAULT-002 | 连接建立超时 | 防火墙阻断对端端口 | 返回超时错误, 资源正确释放 |
| FAULT-003 | 半开连接 | 对端 `kill -9` 后不发 FIN | 心跳/keepalive 检测, 清理连接 |
| FAULT-004 | 网络恢复后重连 | 先 DROP 再 ACCEPT | 自动重连成功, 传输恢复 |

### 3.2 对端进程 Crash

```python
# pytest 测试框架
def test_peer_crash_during_send():
    """对端在接收过程中 crash，发送端应收到错误而非 hang"""
    sender = start_process("sender", "--blocking")
    receiver = start_process("receiver")

    # 等待连接建立
    wait_for_connection(sender, receiver)

    # 开始大消息传输
    trigger_send(sender, size=100*MB)
    time.sleep(0.1)  # 确保传输开始

    # 杀死接收端
    receiver.kill()  # SIGKILL

    # 发送端应在超时内返回错误
    result = sender.wait(timeout=30)
    assert result.returncode != 0 or result.error_detected

    # 验证无资源泄漏
    assert_no_zombie_processes()
```

| 场景 ID | 描述 | 预期行为 |
|---------|------|---------|
| FAULT-005 | 接收端 SIGKILL | 发送端超时检测,错误回调,资源释放 |
| FAULT-006 | 发送端 SIGKILL | 接收端超时检测,不 hang |
| FAULT-007 | 双端同时 crash 后重启 | 无残留状态,可重新建立连接 |

### 3.3 内存不足

| 场景 ID | 描述 | 注入方法 | 预期行为 |
|---------|------|---------|---------|
| FAULT-008 | malloc 失败 | `ulimit -v` 限制 / LD_PRELOAD hook | 返回错误码, 不崩溃, 不 abort |
| FAULT-009 | UCX 内存注册失败 | cgroup memory limit | 降级处理或明确错误 |
| FAULT-010 | 缓冲池耗尽 | 大量并发请求超过池容量 | 背压/等待/错误,不 OOM |

### 3.4 网络抖动/丢包 (TCP 场景)

```bash
# 使用 tc (traffic control) 注入网络故障
# 注入 100ms 延迟 +/- 20ms 抖动
sudo tc qdisc add dev eth0 root netem delay 100ms 20ms

# 注入 5% 丢包
sudo tc qdisc add dev eth0 root netem loss 5%

# 注入 1% 乱序
sudo tc qdisc add dev eth0 root netem reorder 99% gap 5

# 组合: 延迟 + 丢包 + 带宽限制
sudo tc qdisc add dev eth0 root handle 1: netem delay 50ms loss 2%
sudo tc qdisc add dev eth0 parent 1: handle 2: tbf rate 1gbit burst 32kbit latency 400ms
```

| 场景 ID | 注入条件 | 预期行为 |
|---------|---------|---------|
| FAULT-011 | 5% 丢包 | 传输完成,吞吐下降但数据正确 |
| FAULT-012 | 200ms 延迟 | 传输完成,延迟符合预期 |
| FAULT-013 | 50% 丢包 (极端) | 超时后报错或极慢完成,不 hang |
| FAULT-014 | 网络分区 30s | 超时检测,可恢复或报错 |

### 3.5 GPU OOM

| 场景 ID | 描述 | 注入方法 | 预期行为 |
|---------|------|---------|---------|
| FAULT-015 | cudaMalloc 失败 | 预先分配大量 GPU 内存 | 回退到 host 内存或明确错误 |
| FAULT-016 | GPU 内存注册失败 | 超过 BAR1 映射限制 | 错误处理, 不 segfault |
| FAULT-017 | GPU 被其他进程独占 | `CUDA_VISIBLE_DEVICES=""` | 优雅降级到 CPU 路径 |
| FAULT-018 | cudaMemcpy 期间 GPU reset | `nvidia-smi --gpu-reset` | 检测错误, 清理资源 |

---

## 4. CI/CD 建议

### 4.1 测试框架选型

| 框架 | 用途 | 理由 |
|------|------|------|
| **Google Test + Google Mock** | C++ 单元测试 + Mock | HPC 领域事实标准; UCX 自身使用 gtest; GMock 提供强大的 Mock 能力; 与 CMake/CTest 原生集成 |
| **pytest** | Python 接口测试 + 集成测试 | Python 生态最强测试框架; 参数化、fixture、插件丰富; multiprocessing 编排方便 |
| **pytest-benchmark** | Python 性能测试 | 内置统计分析,支持与历史基线对比 |
| **Google Benchmark** | C++ 微基准测试 | 纳秒级精度; 自动预热和统计; 与 gtest 互补 |
| **Catch2** | 备选 (不推荐主用) | 语法更简洁但 Mock 支持弱,HPC 社区采用率低于 gtest |

**推荐组合: Google Test/Mock (C++) + pytest (Python) + Google Benchmark (性能微测)**

### 4.2 CI 环境架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        CI Pipeline                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────┐   ┌──────────────┐   ┌──────────────────────┐ │
│  │  Tier 1     │   │  Tier 2      │   │  Tier 3              │ │
│  │  基础 CI    │   │  硬件 CI     │   │  全量测试            │ │
│  │             │   │              │   │                      │ │
│  │ - 编译检查  │   │ - RDMA 测试  │   │ - 多节点集成测试     │ │
│  │ - 单元测试  │   │ - GPU 测试   │   │ - 性能回归全矩阵     │ │
│  │   (Mock)    │   │ - 性能基线   │   │ - 压力测试 (24h)     │ │
│  │ - 代码风格  │   │              │   │ - 兼容性矩阵         │ │
│  │ - ASAN/TSAN │   │ 触发: 每日   │   │                      │ │
│  │             │   │ 或 PR 标签   │   │ 触发: 发布前/周      │ │
│  │ 触发: 每次  │   │              │   │                      │ │
│  │ Push/PR     │   │ 需要:        │   │ 需要:                │ │
│  │             │   │ - RDMA NIC   │   │ - 多节点集群         │ │
│  │ 需要:       │   │ - GPU        │   │ - 多种 NIC           │ │
│  │ - 普通 x86  │   │              │   │ - 多种 GPU           │ │
│  │ - 无特殊HW  │   │              │   │                      │ │
│  └─────────────┘   └──────────────┘   └──────────────────────┘ │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

#### 4.2.1 Tier 1: 无硬件依赖 CI (每次提交)

**环境**: 标准 x86_64 Linux VM/容器，无 RDMA NIC，无 GPU。

**如何测试无 RDMA 环境下的 UCX 代码:**

1. **Mock UCX 层**: 单元测试完全通过 Mock 运行，零硬件依赖。
2. **UCX TCP/SHM 传输**: UCX 在无 RDMA 硬件时自动回退到 TCP 和共享内存传输，可运行集成测试的核心路径。
3. **SoftRoCE (rxe)**: Linux 内核模块，软件模拟 RoCE 设备，可在普通 VM 中加载。

```bash
# 在 CI 容器中启用 SoftRoCE
sudo modprobe rdma_rxe
sudo rdma link add rxe0 type rxe netdev eth0
# 现在 UCX 可以检测到 RDMA 设备并使用 RC/UD 传输
UCX_TLS=rc,ud,sm,tcp ucx_perftest -t tag_lat localhost
```

4. **构建矩阵**:
```yaml
# .github/workflows/ci-basic.yml
strategy:
  matrix:
    compiler: [gcc-12, gcc-13, clang-17]
    build_type: [Debug, Release, RelWithDebInfo]
    sanitizer: [none, asan, tsan]
```

#### 4.2.2 Tier 2: 硬件 CI (每日/按需)

**环境**: 自托管 Runner，配备 RDMA NIC (ConnectX-6+) 和 GPU (A100+)。

**方案选择:**

| 方案 | 优点 | 缺点 |
|------|------|------|
| 自托管 GitHub Runner | 与 GitHub Actions 集成 | 需自行维护 |
| Jenkins + 专用硬件 | 灵活调度, 硬件标签 | 运维成本高 |
| Slurm 集群集成 | 复用现有 HPC 资源 | 调度延迟不确定 |

**推荐**: 自托管 GitHub Runner (2–4 台)，通过标签 (`rdma`, `gpu`, `multi-node`) 调度。

#### 4.2.3 Tier 3: 全量测试 (发布前)

手动触发或 cron 调度，在完整硬件矩阵上运行所有测试类别。

### 4.3 性能回归检测方案

#### 4.3.1 基线管理

```
perf-baselines/
├── rc_verbs/
│   ├── latency_host_4KB_1.json
│   ├── latency_host_1MB_1.json
│   ├── throughput_host_1MB_64.json
│   └── ...
├── shm/
│   └── ...
└── metadata.json   # 硬件环境描述
```

#### 4.3.2 回归检测算法

```python
def detect_regression(current: PerfResult, baseline: PerfResult,
                      config: RegressionConfig) -> RegressionVerdict:
    """
    使用统计方法检测性能回归,避免噪声导致的误报。
    """
    # 1. 基于相对变化的初步筛选
    latency_change = (current.p50 - baseline.p50) / baseline.p50
    bw_change = (baseline.bandwidth - current.bandwidth) / baseline.bandwidth

    if abs(latency_change) < config.noise_floor:  # 典型值: 2%
        return PASS

    # 2. 统计显著性检测 (Welch's t-test)
    t_stat, p_value = welch_ttest(current.samples, baseline.samples)
    if p_value > config.significance_level:  # 典型值: 0.05
        return PASS  # 差异不显著

    # 3. 与容忍度比较
    if latency_change > config.latency_tolerance:  # 小消息: 10%, 大消息: 5%
        return REGRESSION
    if bw_change > config.bandwidth_tolerance:  # 5%
        return REGRESSION

    return PASS
```

#### 4.3.3 报告与告警

- **Dashboard**: Grafana 展示历史趋势图，每个性能指标一条时间线。
- **PR 评论**: Bot 自动在 PR 上评论性能对比结果，标注回归项。
- **告警**: 回归超过容忍度时阻断 PR 合并 (Tier 2 以上)。

---

## 5. 质量门禁

### 5.1 代码覆盖率目标

| 层级 | 覆盖率目标 (行覆盖) | 覆盖率目标 (分支覆盖) | 说明 |
|------|--------------------|--------------------|------|
| 核心传输逻辑 | >= 85% | >= 75% | endpoint, transport, buffer |
| 错误处理路径 | >= 70% | >= 60% | 故障注入测试覆盖 |
| Python 绑定 | >= 80% | >= 70% | 接口层 |
| 工具/辅助代码 | >= 60% | >= 50% | 配置, 日志等 |
| **总体** | **>= 80%** | **>= 70%** | |

**工具**: gcov/lcov (C++), coverage.py (Python), 通过 Codecov 或 Coveralls 集成到 PR。

**注意**: 覆盖率不包含 Mock 路径本身；硬件相关代码路径 (如 GPUDirect) 在 Tier 2 CI 中统计覆盖率。

### 5.2 性能回归容忍度

| 消息类别 | 延迟容忍度 | 带宽容忍度 | 说明 |
|---------|-----------|-----------|------|
| 小消息 (< 4KB) | +10% 或 +0.5us (取大值) | N/A | 绝对值兜底避免噪声 |
| 中消息 (4KB–256KB) | +8% | -5% | 敏感区间 |
| 大消息 (> 256KB) | +5% | -3% | 接近线速,容忍度更严格 |
| 连接建立 | +20% | N/A | 一次性成本,容忍度宽松 |

**触发策略:**
- 单项超过容忍度 = WARNING (不阻断)
- 3 项以上超过容忍度 = BLOCK (阻断 PR)
- 任意一项超过 2 倍容忍度 = BLOCK

### 5.3 发布前必须通过的测试清单

#### Gate 1: 基础质量 (每次提交)
- [ ] 编译通过 (所有支持的编译器 + Release/Debug)
- [ ] 全量单元测试通过 (gtest)
- [ ] ASAN 构建下无内存错误
- [ ] TSAN 构建下无数据竞争
- [ ] 代码覆盖率达标 (>= 80%)
- [ ] 代码风格检查通过 (clang-format, clang-tidy)
- [ ] Python 单元测试通过 (pytest)

#### Gate 2: 功能验证 (每日)
- [ ] 同节点多进程集成测试通过 (shm + tcp)
- [ ] SoftRoCE 集成测试通过
- [ ] 端到端数据完整性测试通过 (E2E-001 至 E2E-005)
- [ ] Python/C++ 互操作测试通过 (E2E-009)
- [ ] 协议回退测试通过 (E2E-008)

#### Gate 3: 硬件验证 (发布前)
- [ ] RDMA (IB/RoCE) 传输功能测试通过
- [ ] GPU 内存传输测试通过 (E2E-006, E2E-007)
- [ ] HCCL/NCCL 扩展测试通过 (E2E-010)
- [ ] 性能基线测试通过 (全矩阵 P0 + P1)
- [ ] 性能回归检测通过 (无阻断级别回归)

#### Gate 4: 稳定性验证 (发布前)
- [ ] 24 小时持续传输压力测试通过
- [ ] 1000 并发连接测试通过
- [ ] 内存泄漏检测通过 (Valgrind/ASAN 长时间运行)
- [ ] 故障注入测试通过 (FAULT-001 至 FAULT-018)

#### Gate 5: 兼容性验证 (大版本发布前)
- [ ] Pairwise 兼容性矩阵测试通过 (20+ 组合)
- [ ] UCX 版本兼容测试通过 (最近 4 个 minor 版本)
- [ ] CUDA 版本兼容测试通过 (最近 3 个版本)
- [ ] Python 版本兼容测试通过 (3.9–3.12)

### 5.4 质量指标仪表板

```
┌─────────────────────────────────────────────────────┐
│              P2P 库质量仪表板                         │
├──────────────────┬──────────────────────────────────┤
│ 编译状态         │  ● GCC12 ● GCC13 ● Clang17      │
│ 单元测试         │  ● 342/342 passed                │
│ 集成测试         │  ● 48/48 passed                  │
│ 代码覆盖率       │  ● 83.2% (行) / 72.1% (分支)    │
│ ASAN             │  ● 0 errors                      │
│ TSAN             │  ● 0 warnings                    │
│ 性能回归         │  ● 0 regressions / 2 warnings    │
│ 压力测试 (最近)  │  ● 24h passed (3 天前)           │
│ 故障测试         │  ● 18/18 passed                  │
└──────────────────┴──────────────────────────────────┘
```

---

## 6. 参考资料

### 性能数据来源
- [UCX Performance Measurement Wiki](https://github.com/openucx/ucx/wiki/Performance-measurement)
- [NCCL Tests Performance Documentation](https://github.com/NVIDIA/nccl-tests/blob/master/doc/PERFORMANCE.md)
- [NVIDIA Multi-Node Tuning Guide - UCX](https://docs.nvidia.com/multi-node-nvlink-systems/multi-node-tuning-guide/ucx.html)
- [NVIDIA Multi-Node Tuning Guide - NCCL](https://docs.nvidia.com/multi-node-nvlink-systems/multi-node-tuning-guide/nccl.html)
- [Introducing 200G HDR InfiniBand Solutions (NVIDIA Whitepaper)](https://network.nvidia.com/files/doc-2020/wp-introducing-200g-hdr-infiniband-solutions.pdf)
- [RoCE v2 vs InfiniBand Comparison](https://dataoorts.com/roce-v2-vs-infiniband-compare-for-gpu-clusters/)
- [IPC Benchmarks (goldsborough/ipc-bench)](https://github.com/goldsborough/ipc-bench)

### 测试框架与方法论
- [Google RDMA Unit Tests](https://github.com/google/rdma-unit-test)
- [GoogleTest User Guide](https://google.github.io/googletest/)
- [Testing HPC C++ with GoogleTest + MPI (ResearchGate)](https://www.researchgate.net/publication/355211588_Testing_HPC_C_software_with_GoogleTest_adjusting_the_test_framework_for_distributed_parallel_tests_using_MPI)
- [linux-rdma/perftest (IB Verbs Performance Tests)](https://github.com/linux-rdma/perftest)
- [Verifying HPC Applications with CI (ACM)](https://dl.acm.org/doi/fullHtml/10.1145/3569951.3597557)

### HPC CI/CD 实践
- [NCCL Performance Testing & Tuning Guide](https://ai-hpc.org/en/guide/03-network/nccl-test)
- [Practitioner's Guide to Testing Large GPU Clusters (Together AI)](https://www.together.ai/blog/a-practitioners-guide-to-testing-and-running-large-gpu-clusters-for-training-generative-ai-models)
- [OpenUCX FAQ - Transport Configuration](https://openucx.readthedocs.io/en/master/faq.html)
- [Scalability of OFI and UCX on ARCHER2 (CUG 2024)](https://cug.org/proceedings/cug2024_proceedings/includes/files/pap143s2-file1.pdf)
