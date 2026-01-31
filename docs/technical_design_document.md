# ZeroKV 技术设计文档

版本: 1.0
日期: 2025-01-31
状态: 设计阶段

## 1. 项目概述

### 1.1 项目背景

在分布式NPU训练和推理场景中，需要高效管理和传输NPU显存中的模型参数、梯度、中间激活值等数据。传统方案需要将数据从NPU拷贝到Host DDR，再通过网络传输，存在以下问题：

- 多次内存拷贝造成延迟增加
- Host DDR成为带宽瓶颈
- CPU参与数据搬运，占用计算资源

### 1.2 项目目标

设计并实现一个高性能NPU显存KV管理中间件，实现：

1. **零拷贝传输**: NPU间直接内存传输，无需通过Host DDR
2. **高性能**: 延迟 <50μs, 带宽 >180GB/s
3. **易用性**: 提供C++和Python接口
4. **可扩展**: 支持多节点分布式部署
5. **可观测**: 完整的性能监控方案

### 1.3 技术选型

| 组件 | 技术选择 | 理由 |
|------|---------|------|
| 数据平面 | P2P-Transfer | NPU间零拷贝，支持HCCS/RoCE |
| 控制平面 | UCX | 多传输层支持，高性能RPC |
| 序列化 | Protobuf | 高效、跨语言 |
| 监控 | Prometheus + Grafana | 业界标准，生态完善 |
| Python绑定 | pybind11 | 轻量、性能好 |

## 2. 系统架构

### 2.1 整体架构

```
┌──────────────────────────────────────────────────────────┐
│                      Client Application                   │
│              (C++ API / Python API)                      │
└────────────────────┬─────────────────────────────────────┘
                     │
        ┌────────────┴────────────┐
        │                         │
┌───────▼────────┐        ┌──────▼──────┐
│  ZeroKVClient   │        │ ZeroKVServer │
│   (Device 1)   │        │ (Device 0)  │
└───────┬────────┘        └──────┬──────┘
        │                         │
        │   ┌─────────────────────┤
        │   │   Control Plane     │
        │   │   (UCX RPC)         │
        │   │   - Metadata        │
        │   │   - KV Lookup       │
        │   └─────────────────────┤
        │                         │
        │   ┌─────────────────────┤
        │   │   Data Plane        │
        └───┤   (P2P-Transfer)    │
            │   - Zero-copy       │
            │   - HCCS/RoCE       │
            └─────────────────────┘
                     │
        ┌────────────┴────────────┐
        │                         │
┌───────▼────────┐        ┌──────▼──────┐
│   NPU Memory   │        │ NPU Memory  │
│   (Device 1)   │◄──────►│ (Device 0)  │
└────────────────┘        └─────────────┘
      Direct Memory Transfer (HCCS/RoCE)
```

### 2.2 双平面设计

**控制平面 (UCX)**:
- 处理Put/Get/Delete请求
- 管理KV元数据 (key → {devPtr, size, deviceId})
- 交换P2P连接信息 (HcclRootInfo)
- 轻量级、低延迟

**数据平面 (P2P-Transfer)**:
- NPU显存直接传输
- 支持HCCS (同节点) 和 RoCE (跨节点)
- 零拷贝、高带宽
- 无需CPU/Host DDR参与

### 2.3 分层设计

```
┌─────────────────────────────────────┐
│     Python Layer (Optional)         │  ← pybind11绑定
├─────────────────────────────────────┤
│     C++ API Layer                   │  ← 用户API
│  ZeroKVServer / ZeroKVClient          │
├─────────────────────────────────────┤
│     Control Plane                   │  ← UCX RPC
│  UCXControlServer / UCXControlClient│
├─────────────────────────────────────┤
│     Data Plane                      │  ← P2P-Transfer
│  P2PComm / HcclRootInfo             │
├─────────────────────────────────────┤
│     Monitoring Layer                │  ← 性能监控
│  PerformanceMonitor / Metrics       │
├─────────────────────────────────────┤
│     NPU Runtime                     │  ← ACL/HCCL
│  aclrt* / hccl*                     │
└─────────────────────────────────────┘
```

## 3. 核心模块设计

### 3.1 ZeroKVServer

**职责**:
- 管理KV元数据表
- 处理客户端RPC请求
- 管理P2P连接池
- 记录性能指标

**关键数据结构**:

```cpp
struct KVMetadata {
    void* devPtr;          // NPU显存地址
    uint64_t size;         // 数据大小
    uint32_t deviceId;     // NPU设备ID
    HcclDataType dataType; // 数据类型
    std::string ownerId;   // 所属客户端ID
    uint64_t timestamp;    // 注册时间戳
};

class ZeroKVServer {
private:
    // KV元数据表
    std::unordered_map<std::string, KVMetadata> kvTable_;
    std::shared_mutex kvTableMutex_;

    // P2P连接池
    std::unordered_map<std::string, P2PComm> clientP2PConns_;
    std::mutex connMutex_;

    // UCX控制服务器
    std::unique_ptr<UCXControlServer> ucxServer_;

    // 性能监控
    std::shared_ptr<PerformanceMonitor> monitor_;
};
```

**主要接口**:

```cpp
Status Start(const std::string& ip, uint16_t port);
Status Put(const std::string& key, void* devPtr, size_t size,
           HcclDataType dataType);
Status Delete(const std::string& key);
Status Shutdown();
```

### 3.2 ZeroKVClient

**职责**:
- 连接到KVServer
- 发起Get请求并接收数据
- 管理本地P2P接收端
- 支持同步和异步API

**关键数据结构**:

```cpp
class ZeroKVClient {
private:
    uint32_t deviceId_;
    std::string clientId_;

    // UCX控制客户端
    std::unique_ptr<UCXControlClient> ucxClient_;

    // 本地P2P通信上下文
    P2PComm localP2PComm_;
    HcclRootInfo localRootInfo_;

    // 性能监控
    std::shared_ptr<PerformanceMonitor> monitor_;
};
```

**主要接口**:

```cpp
Status Connect(const std::string& serverIp, uint16_t port);
Status Get(const std::string& key, void* localDevPtr, size_t size);
Status GetAsync(const std::string& key, void* localDevPtr, size_t size,
                aclrtStream stream);
Status Disconnect();
```

### 3.3 UCX控制平面

**UCXControlServer**:

```cpp
class UCXControlServer {
public:
    Status Init(const std::string& ip, uint16_t port);

    // RPC处理器
    void RegisterPutHandler(PutHandler handler);
    void RegisterGetHandler(GetHandler handler);
    void RegisterDeleteHandler(DeleteHandler handler);

    Status Start();
    Status Stop();

private:
    ucp_context_h ucpContext_;
    ucp_worker_h ucpWorker_;
    ucp_listener_h ucpListener_;

    std::unordered_map<std::string, ucp_ep_h> clientEndpoints_;
};
```

**UCXControlClient**:

```cpp
class UCXControlClient {
public:
    Status Connect(const std::string& serverIp, uint16_t port);

    Status SendPutRequest(const PutRequest& req, PutResponse& resp);
    Status SendGetRequest(const GetRequest& req, GetResponse& resp);
    Status SendDeleteRequest(const DeleteRequest& req, DeleteResponse& resp);

    Status Disconnect();

private:
    ucp_context_h ucpContext_;
    ucp_worker_h ucpWorker_;
    ucp_ep_h serverEndpoint_;
};
```

### 3.4 P2P数据平面

**核心流程**:

1. **连接建立** (通过控制平面):
   ```
   Client                    Server
      │                         │
      │  GetRequest             │
      ├────────────────────────>│
      │                         │ P2PGetRootInfo
      │                         │ (获取Server的RootInfo)
      │  GetResponse            │
      │  (含Server RootInfo)    │
      │<────────────────────────┤
      │                         │
   P2PCommInitRootInfo      P2PCommInitRootInfo
   (初始化P2P连接)          (初始化P2P连接)
      │                         │
   ```

2. **数据传输**:
   ```
   Client                    Server
      │                         │
      │  P2PSend (from Server)  │
      │<════════════════════════│
      │   (NPU Memory Direct)   │
      │                         │
   P2PRecv                      │
   (写入Client NPU内存)         │
   ```

**P2P Mock实现** (基于UCX):

```cpp
class P2PMockContext {
public:
    ucp_context_h ucpContext;
    ucp_worker_h ucpWorker;
    ucp_address_t* workerAddr;
    size_t workerAddrLen;

    // 模拟NPU内存
    std::unordered_map<uint64_t, std::vector<uint8_t>> mockDeviceMemory;
};

HcclResult P2PSend(P2PComm comm, void* sendBuf, size_t sendBytes,
                   uint32_t destRank);
HcclResult P2PRecv(P2PComm comm, void* recvBuf, size_t recvBytes,
                   uint32_t srcRank, aclrtStream stream);
```

### 3.5 性能监控

**PerformanceMonitor**:

```cpp
struct OperationMetrics {
    OperationType type;      // PUT/GET/DELETE/P2P_SEND/P2P_RECV
    uint64_t latencyUs;      // 延迟(微秒)
    uint64_t dataSize;       // 数据大小(字节)
    uint64_t timestamp;      // 时间戳
    bool success;            // 是否成功
};

class PerformanceMonitor {
public:
    void RecordOperation(const OperationMetrics& metrics);

    AggregatedStats GetStats(OperationType type) const;
    std::string ExportPrometheus() const;

    void StartRealTimeDisplay();
    void StopRealTimeDisplay();

private:
    mutable std::shared_mutex metricsMutex_;
    std::map<OperationType, std::vector<OperationMetrics>> metricsHistory_;

    // 聚合统计
    std::map<OperationType, AggregatedStats> aggregatedStats_;
};

struct AggregatedStats {
    uint64_t totalOps;
    uint64_t successOps;
    uint64_t failedOps;
    double avgLatencyUs;
    double p50LatencyUs;
    double p95LatencyUs;
    double p99LatencyUs;
    double throughputMBps;
};
```

**Prometheus Exporter**:

```cpp
class PrometheusExporter {
public:
    Status Start(uint16_t port);  // 默认9090

private:
    void HandleMetricsRequest(HttpRequest& req, HttpResponse& resp);
    std::string GenerateMetrics();

    std::shared_ptr<PerformanceMonitor> monitor_;
    std::unique_ptr<HttpServer> httpServer_;
};
```

## 4. API规范

### 4.1 C++ API

**Server端**:

```cpp
#include "zerokv/zerokv_server.h"

// 初始化Server
ZeroKVServer server(0);  // Device ID = 0
auto status = server.Start("0.0.0.0", 50051);

// 注册NPU显存
float* devPtr;
aclrtMalloc((void**)&devPtr, 1024 * sizeof(float), ACL_MEM_MALLOC_HUGE_FIRST);
status = server.Put("layer1_weights", devPtr, 1024 * sizeof(float),
                    HCCL_DATA_TYPE_FP32);

// 删除KV
status = server.Delete("layer1_weights");

// 关闭Server
server.Shutdown();
```

**Client端**:

```cpp
#include "zerokv/zerokv_client.h"

// 连接Server
ZeroKVClient client(1);  // Device ID = 1
auto status = client.Connect("192.168.1.100", 50051);

// 同步获取
float* localDevPtr;
aclrtMalloc((void**)&localDevPtr, 1024 * sizeof(float), ACL_MEM_MALLOC_HUGE_FIRST);
status = client.Get("layer1_weights", localDevPtr, 1024 * sizeof(float));

// 异步获取
aclrtStream stream;
aclrtCreateStream(&stream);
status = client.GetAsync("layer1_weights", localDevPtr, 1024 * sizeof(float), stream);
aclrtSynchronizeStream(stream);

// 断开连接
client.Disconnect();
```

### 4.2 Python API

**Server端**:

```python
import zerokv

# 初始化Server
server = zerokv.Server(device_id=0)
server.start("0.0.0.0", 50051)

# 注册NumPy数组
import numpy as np
data = np.random.randn(1024, 1024).astype(np.float32)
server.put("embeddings", data)

# 注册PyTorch Tensor
import torch
tensor = torch.randn(1024, 1024, device='npu:0')
server.put("model_state", tensor)

# 删除KV
server.delete("embeddings")

# 关闭Server
server.shutdown()
```

**Client端**:

```python
import zerokv

# 连接Server
client = zerokv.Client(device_id=1)
client.connect("192.168.1.100", 50051)

# 获取为NumPy数组
result = client.get("embeddings")
print(result.shape)  # (1024, 1024)

# 获取为PyTorch Tensor
tensor = client.get_torch("model_state")
print(tensor.device)  # npu:1

# 断开连接
client.disconnect()
```

### 4.3 性能监控API

**C++**:

```cpp
#include "zerokv/zerokv_monitor.h"

auto monitor = server.GetMonitor();

// 获取统计信息
auto stats = monitor->GetStats(OperationType::GET);
std::cout << "P95 Latency: " << stats.p95LatencyUs << " us\n";
std::cout << "Throughput: " << stats.throughputMBps << " MB/s\n";

// 启动实时显示
monitor->StartRealTimeDisplay();

// 导出Prometheus格式
std::string metrics = monitor->ExportPrometheus();
```

**Python**:

```python
import zerokv

monitor = server.get_monitor()

# 获取统计信息
stats = monitor.get_stats('GET')
print(f"P95 Latency: {stats['p95_latency_us']} us")
print(f"Throughput: {stats['throughput_mbps']} MB/s")

# 启动实时显示
monitor.start_display()

# 获取完整统计
all_stats = monitor.get_all_stats()
```

## 5. 数据流详解

### 5.1 Put操作流程

```
Application                Client               Server
     │                        │                    │
     │  put(key, devPtr)      │                    │
     ├───────────────────────>│                    │
     │                        │  PutRequest(RPC)   │
     │                        │  {key, devPtr,     │
     │                        │   size, deviceId}  │
     │                        ├───────────────────>│
     │                        │                    │ Insert to kvTable_
     │                        │                    │ {key → metadata}
     │                        │  PutResponse       │
     │                        │  {status: OK}      │
     │                        │<───────────────────┤
     │  Status::OK            │                    │
     │<───────────────────────┤                    │
```

注意: Put操作仅注册元数据，不传输数据。devPtr仍在Server的NPU内存中。

### 5.2 Get操作流程

```
Client                         Server
  │                               │
  │  GetRequest(RPC)              │
  │  {key, clientRootInfo}        │
  ├──────────────────────────────>│
  │                               │ Lookup kvTable_[key]
  │                               │ → {devPtr, size, ...}
  │                               │
  │                               │ P2PGetRootInfo
  │                               │ → serverRootInfo
  │  GetResponse                  │
  │  {devPtr, size, serverRootInfo}│
  │<──────────────────────────────┤
  │                               │
P2PCommInitRootInfo           P2PCommInitRootInfo
(建立P2P连接)                 (建立P2P连接)
  │                               │
  │                               │ P2PSend
  │  ═══════════════════════════> │ (devPtr → network)
  │   (NPU Direct Memory)         │
P2PRecv                           │
(写入localDevPtr)                 │
  │                               │
```

### 5.3 异步Get流程

```
Client                         Server
  │                               │
  │  GetAsyncRequest              │
  ├──────────────────────────────>│
  │  GetResponse                  │
  │<──────────────────────────────┤
  │                               │
  │  P2PRecv(stream)              │
  │  (non-blocking)               │
  │<═══════════════════════════════│
  │                               │
  │  // Application continues     │
  │  do_other_work();             │
  │                               │
  │  aclrtSynchronizeStream       │
  │  (等待传输完成)               │
  │                               │
```

## 6. 性能优化策略

### 6.1 零拷贝路径

传统方案:
```
NPU0 → Host DDR → Network → Host DDR → NPU1
     (拷贝1)    (拷贝2)    (拷贝3)    (拷贝4)
```

本方案:
```
NPU0 → HCCS/RoCE → NPU1
     (零拷贝，DMA直传)
```

### 6.2 连接池复用

- 维护 `clientId → P2PComm` 映射
- 首次连接建立P2P，后续复用
- 减少连接建立开销 (~10ms → 0)

### 6.3 批量操作

支持批量Get:

```cpp
Status BatchGet(const std::vector<std::string>& keys,
                std::vector<void*>& devPtrs,
                std::vector<size_t>& sizes);
```

通过P2PScatterFromRemoteHostMem一次传输多个KV。

### 6.4 内存对齐

确保NPU内存对齐到512字节边界，提升DMA效率:

```cpp
aclrtMalloc(&devPtr, alignedSize, ACL_MEM_MALLOC_HUGE_FIRST);
```

### 6.5 NUMA感知

在多NPU节点上，优先使用本地NPU:

```cpp
uint32_t GetOptimalDevice(const std::string& key) {
    // Hash key到本地NUMA节点的NPU
    return HashToLocalDevice(key);
}
```

## 7. 容错设计

### 7.1 连接重试

UCX连接失败时，指数退避重试:

```cpp
Status ConnectWithRetry(const std::string& ip, uint16_t port,
                       int maxRetries = 3) {
    for (int i = 0; i < maxRetries; ++i) {
        auto status = Connect(ip, port);
        if (status.ok()) return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(100 * (1 << i)));
    }
    return Status::Error("Max retries exceeded");
}
```

### 7.2 传输超时

P2P传输设置超时:

```cpp
Status GetWithTimeout(const std::string& key, void* localDevPtr,
                     size_t size, int timeoutMs = 5000) {
    auto future = std::async(std::launch::async,
                            [&]() { return Get(key, localDevPtr, size); });
    if (future.wait_for(std::chrono::milliseconds(timeoutMs)) ==
        std::future_status::timeout) {
        return Status::Error("Get operation timeout");
    }
    return future.get();
}
```

### 7.3 内存保护

防止非法内存访问:

```cpp
Status ValidateDevicePointer(void* devPtr, size_t size) {
    // 检查地址是否在已注册的NPU内存范围内
    aclrtMemoryInfo info;
    auto ret = aclrtMemGetInfo(devPtr, &info);
    if (ret != ACL_SUCCESS || info.size < size) {
        return Status::Error("Invalid device pointer");
    }
    return Status::OK();
}
```

### 7.4 优雅关闭

Server关闭时，通知所有Client:

```cpp
Status Shutdown() {
    // 停止接受新连接
    ucxServer_->StopAccepting();

    // 通知所有Client
    for (auto& [clientId, ep] : clientEndpoints_) {
        SendShutdownNotification(ep);
    }

    // 等待所有传输完成
    WaitForPendingTransfers();

    // 清理资源
    CleanupResources();
}
```

## 8. 监控告警

### 8.1 Prometheus指标

```
# HELP zerokv_operation_duration_seconds Operation latency
# TYPE zerokv_operation_duration_seconds histogram
zerokv_operation_duration_seconds_bucket{operation="get",le="0.00005"} 1850
zerokv_operation_duration_seconds_bucket{operation="get",le="0.0001"} 1980
zerokv_operation_duration_seconds_bucket{operation="get",le="+Inf"} 2000

# HELP zerokv_operation_total Total operations
# TYPE zerokv_operation_total counter
zerokv_operation_total{operation="get",status="success"} 1980
zerokv_operation_total{operation="get",status="failure"} 20

# HELP zerokv_throughput_bytes_per_second Data transfer throughput
# TYPE zerokv_throughput_bytes_per_second gauge
zerokv_throughput_bytes_per_second{operation="get"} 193273528320

# HELP zerokv_active_connections Active client connections
# TYPE zerokv_active_connections gauge
zerokv_active_connections 15
```

### 8.2 告警规则

**高延迟告警**:
```yaml
- alert: HighLatency
  expr: histogram_quantile(0.95, zerokv_operation_duration_seconds) > 0.001
  for: 5m
  labels:
    severity: warning
  annotations:
    summary: "P95 latency > 1ms"
```

**低吞吐告警**:
```yaml
- alert: LowThroughput
  expr: rate(zerokv_operation_total[1m]) < 100
  for: 5m
  labels:
    severity: warning
  annotations:
    summary: "QPS < 100"
```

**高错误率告警**:
```yaml
- alert: HighErrorRate
  expr: rate(zerokv_operation_total{status="failure"}[5m]) /
        rate(zerokv_operation_total[5m]) > 0.05
  for: 2m
  labels:
    severity: critical
  annotations:
    summary: "Error rate > 5%"
```

### 8.3 Grafana仪表盘

面板包括:

1. **QPS面板**: 实时QPS曲线 (Put/Get/Delete)
2. **延迟分布**: P50/P95/P99延迟
3. **带宽利用率**: 实时传输速率
4. **错误率**: 失败操作占比
5. **连接数**: 活跃连接数
6. **热力图**: 延迟随时间分布

## 9. 部署方案

### 9.1 单机部署

```bash
# 编译
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# 启动Server
./bin/zerokv_server --device_id=0 --port=50051 --monitor_port=9090

# 启动监控
cd monitoring
docker-compose up -d
```

### 9.2 分布式部署

**节点1 (Server)**:
```bash
# 启动Server
./bin/zerokv_server \
    --device_id=0 \
    --ip=192.168.1.100 \
    --port=50051 \
    --monitor_port=9090
```

**节点2-N (Client)**:
```bash
# 应用程序连接Server
ZeroKVClient client(0);
client.Connect("192.168.1.100", 50051);
```

**监控节点**:
```bash
# Prometheus配置
scrape_configs:
  - job_name: 'zerokv-servers'
    static_configs:
      - targets:
        - '192.168.1.100:9090'
        - '192.168.1.101:9090'
```

### 9.3 Kubernetes部署

```yaml
apiVersion: apps/v1
kind: StatefulSet
metadata:
  name: zerokv-server
spec:
  replicas: 3
  selector:
    matchLabels:
      app: zerokv-server
  template:
    metadata:
      labels:
        app: zerokv-server
    spec:
      containers:
      - name: zerokv-server
        image: zerokv-server:latest
        resources:
          limits:
            ascend.com/npu: 1
        ports:
        - containerPort: 50051
          name: rpc
        - containerPort: 9090
          name: metrics
```

## 10. 测试方案

### 10.1 单元测试

使用Google Test框架:

```cpp
TEST(ZeroKVServerTest, PutAndGet) {
    ZeroKVServer server(0);
    server.Start("127.0.0.1", 50051);

    float* devPtr;
    aclrtMalloc((void**)&devPtr, 1024, ACL_MEM_MALLOC_HUGE_FIRST);

    auto status = server.Put("test_key", devPtr, 1024);
    EXPECT_TRUE(status.ok());

    ZeroKVClient client(0);
    client.Connect("127.0.0.1", 50051);

    float* localPtr;
    aclrtMalloc((void**)&localPtr, 1024, ACL_MEM_MALLOC_HUGE_FIRST);

    status = client.Get("test_key", localPtr, 1024);
    EXPECT_TRUE(status.ok());
}
```

### 10.2 集成测试

测试跨NPU传输:

```cpp
TEST(IntegrationTest, CrossDeviceTransfer) {
    // Device 0 作为Server
    ZeroKVServer server(0);
    server.Start("127.0.0.1", 50051);

    float* serverDevPtr;
    aclrtSetDevice(0);
    aclrtMalloc((void**)&serverDevPtr, 4096, ACL_MEM_MALLOC_HUGE_FIRST);

    // 填充测试数据
    std::vector<float> testData(1024, 3.14f);
    aclrtMemcpy(serverDevPtr, 4096, testData.data(), 4096,
                ACL_MEMCPY_HOST_TO_DEVICE);

    server.Put("cross_device_test", serverDevPtr, 4096);

    // Device 1 作为Client
    ZeroKVClient client(1);
    client.Connect("127.0.0.1", 50051);

    float* clientDevPtr;
    aclrtSetDevice(1);
    aclrtMalloc((void**)&clientDevPtr, 4096, ACL_MEM_MALLOC_HUGE_FIRST);

    auto status = client.Get("cross_device_test", clientDevPtr, 4096);
    EXPECT_TRUE(status.ok());

    // 验证数据
    std::vector<float> resultData(1024);
    aclrtMemcpy(resultData.data(), 4096, clientDevPtr, 4096,
                ACL_MEMCPY_DEVICE_TO_HOST);

    EXPECT_EQ(resultData, testData);
}
```

### 10.3 性能测试

基准测试:

```cpp
void BenchmarkGet(benchmark::State& state) {
    ZeroKVServer server(0);
    server.Start("127.0.0.1", 50051);

    ZeroKVClient client(1);
    client.Connect("127.0.0.1", 50051);

    size_t dataSize = state.range(0);

    for (auto _ : state) {
        client.Get("bench_key", localDevPtr, dataSize);
    }

    state.SetBytesProcessed(state.iterations() * dataSize);
}

BENCHMARK(BenchmarkGet)->Range(1024, 1<<30);  // 1KB to 1GB
```

### 10.4 压力测试

多客户端并发测试:

```python
import zerokv
import concurrent.futures
import time

def stress_test_worker(client_id, num_ops):
    client = zerokv.Client(device_id=client_id % 8)
    client.connect("192.168.1.100", 50051)

    start = time.time()
    for i in range(num_ops):
        result = client.get("shared_key")
    elapsed = time.time() - start

    print(f"Client {client_id}: {num_ops/elapsed:.2f} ops/s")

# 100个并发客户端
with concurrent.futures.ThreadPoolExecutor(max_workers=100) as executor:
    futures = [executor.submit(stress_test_worker, i, 1000)
               for i in range(100)]
    concurrent.futures.wait(futures)
```

## 11. 性能目标

| 指标 | 目标 | 测试条件 |
|------|------|---------|
| Get延迟 (P50) | <30μs | 1MB数据, 同节点 |
| Get延迟 (P95) | <50μs | 1MB数据, 同节点 |
| Get延迟 (P99) | <100μs | 1MB数据, 同节点 |
| Put延迟 | <10μs | 仅元数据注册 |
| 吞吐量 (QPS) | >5M ops/s | 1KB数据 |
| 带宽 (Get) | >180GB/s | 大块传输, HCCS |
| 带宽 (Get) | >100GB/s | 大块传输, RoCE |
| 并发连接数 | >1000 | 单Server |
| 内存开销 | <100MB | Server端 |

## 12. 风险评估

| 风险 | 可能性 | 影响 | 缓解措施 |
|------|-------|------|---------|
| P2P-Transfer API不稳定 | 中 | 高 | 提供UCX Mock实现 |
| NPU硬件资源不足 | 高 | 中 | Mock模式开发, CI/CD使用CPU |
| UCX性能不达标 | 低 | 中 | 性能测试早发现, 可切换到gRPC |
| 内存泄漏 | 中 | 高 | Valgrind检测, RAII管理资源 |
| 网络抖动导致超时 | 中 | 中 | 重试机制, 超时告警 |
| 大规模并发崩溃 | 低 | 高 | 压力测试, 限流保护 |

## 13. 未来演进

### 13.1 分布式扩展

支持多Server分片:

```
Client → [Consistent Hash] → Server1 (keys: a-m)
                           → Server2 (keys: n-z)
```

### 13.2 持久化

支持将KV元数据持久化到RocksDB:

```cpp
class PersistentKVTable {
    rocksdb::DB* db_;

    Status Put(const std::string& key, const KVMetadata& meta) {
        std::string value = SerializeMetadata(meta);
        return db_->Put(rocksdb::WriteOptions(), key, value);
    }
};
```

### 13.3 副本机制

支持多副本提高可用性:

```cpp
Status PutWithReplication(const std::string& key, void* devPtr, size_t size,
                         int numReplicas = 3) {
    // 写入主副本
    Put(key, devPtr, size);

    // 异步复制到备份Server
    for (int i = 0; i < numReplicas - 1; ++i) {
        ReplicateAsync(replicaServers_[i], key, devPtr, size);
    }
}
```

### 13.4 智能预取

基于访问模式预取:

```cpp
class PrefetchEngine {
    void LearnAccessPattern(const std::vector<std::string>& accessSeq);
    std::vector<std::string> PredictNextKeys(const std::string& currentKey);
};
```

## 14. 总结

ZeroKV通过双平面架构实现了高性能、低延迟的NPU显存管理:

- **零拷贝**: P2P-Transfer直接传输NPU内存
- **高性能**: 延迟<50μs, 带宽>180GB/s
- **易用**: C++/Python API, 简单集成
- **可观测**: 完整的Prometheus + Grafana监控
- **灵活**: Mock模式支持无NPU环境开发

项目采用模块化设计，便于扩展和维护，为分布式NPU应用提供了坚实的基础设施。
