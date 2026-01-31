# ZeroKV API Reference

版本: 1.0

## C++ API

### ZeroKVServer

服务端主类，管理KV存储和客户端连接。

#### 头文件

```cpp
#include "zerokv/zerokv_server.h"
```

#### 构造函数

```cpp
explicit ZeroKVServer(uint32_t deviceId);
```

**参数**:
- `deviceId`: NPU设备ID (0-7)

**示例**:
```cpp
ZeroKVServer server(0);  // 使用Device 0
```

#### Start

启动服务器并开始监听。

```cpp
Status Start(const std::string& ip, uint16_t port);
```

**参数**:
- `ip`: 监听IP地址 ("0.0.0.0"表示所有接口)
- `port`: 监听端口 (推荐50051)

**返回**: `Status::OK()` 成功, 否则返回错误信息

**示例**:
```cpp
auto status = server.Start("0.0.0.0", 50051);
if (!status.ok()) {
    LOG(ERROR) << "Failed to start server: " << status.message();
}
```

#### Put

注册NPU显存到KV存储。

```cpp
Status Put(const std::string& key,
           void* devPtr,
           size_t size,
           HcclDataType dataType = HCCL_DATA_TYPE_FP32);
```

**参数**:
- `key`: 键名 (唯一标识符)
- `devPtr`: NPU显存地址 (通过aclrtMalloc分配)
- `size`: 数据大小 (字节)
- `dataType`: 数据类型 (默认FP32)

**返回**: `Status::OK()` 成功, 否则返回错误信息

**注意**:
- 此操作仅注册元数据，不拷贝数据
- devPtr必须是有效的NPU设备内存
- key必须唯一，重复Put会覆盖

**示例**:
```cpp
float* devPtr;
aclrtMalloc((void**)&devPtr, 1024 * sizeof(float), ACL_MEM_MALLOC_HUGE_FIRST);

// 填充数据
std::vector<float> hostData(1024, 1.0f);
aclrtMemcpy(devPtr, 1024 * sizeof(float), hostData.data(),
            1024 * sizeof(float), ACL_MEMCPY_HOST_TO_DEVICE);

// 注册到KV存储
auto status = server.Put("layer1_weights", devPtr, 1024 * sizeof(float));
```

#### Delete

从KV存储中删除键。

```cpp
Status Delete(const std::string& key);
```

**参数**:
- `key`: 要删除的键名

**返回**: `Status::OK()` 成功, 否则返回错误信息

**注意**: 仅删除元数据，不释放NPU内存

**示例**:
```cpp
auto status = server.Delete("layer1_weights");
```

#### GetMonitor

获取性能监控对象。

```cpp
std::shared_ptr<PerformanceMonitor> GetMonitor();
```

**返回**: 性能监控对象指针

**示例**:
```cpp
auto monitor = server.GetMonitor();
auto stats = monitor->GetStats(OperationType::GET);
std::cout << "P95 Latency: " << stats.p95LatencyUs << " us\n";
```

#### Shutdown

关闭服务器并释放资源。

```cpp
Status Shutdown();
```

**返回**: `Status::OK()` 成功, 否则返回错误信息

**示例**:
```cpp
server.Shutdown();
```

---

### ZeroKVClient

客户端类，用于从服务器获取数据。

#### 头文件

```cpp
#include "zerokv/zerokv_client.h"
```

#### 构造函数

```cpp
explicit ZeroKVClient(uint32_t deviceId);
```

**参数**:
- `deviceId`: 本地NPU设备ID

**示例**:
```cpp
ZeroKVClient client(1);  // 使用Device 1
```

#### Connect

连接到服务器。

```cpp
Status Connect(const std::string& serverIp, uint16_t port);
```

**参数**:
- `serverIp`: 服务器IP地址
- `port`: 服务器端口

**返回**: `Status::OK()` 成功, 否则返回错误信息

**示例**:
```cpp
auto status = client.Connect("192.168.1.100", 50051);
if (!status.ok()) {
    LOG(ERROR) << "Failed to connect: " << status.message();
}
```

#### Get

同步获取数据到本地NPU内存。

```cpp
Status Get(const std::string& key,
           void* localDevPtr,
           size_t size);
```

**参数**:
- `key`: 键名
- `localDevPtr`: 本地NPU内存地址 (预先分配)
- `size`: 数据大小 (必须匹配Server端注册的size)

**返回**: `Status::OK()` 成功, 否则返回错误信息

**注意**:
- 此操作会阻塞直到传输完成
- localDevPtr必须提前通过aclrtMalloc分配
- size必须与Put时的size一致

**示例**:
```cpp
float* localDevPtr;
aclrtMalloc((void**)&localDevPtr, 1024 * sizeof(float), ACL_MEM_MALLOC_HUGE_FIRST);

auto status = client.Get("layer1_weights", localDevPtr, 1024 * sizeof(float));

// 使用数据
std::vector<float> result(1024);
aclrtMemcpy(result.data(), 1024 * sizeof(float), localDevPtr,
            1024 * sizeof(float), ACL_MEMCPY_DEVICE_TO_HOST);
```

#### GetAsync

异步获取数据到本地NPU内存。

```cpp
Status GetAsync(const std::string& key,
                void* localDevPtr,
                size_t size,
                aclrtStream stream);
```

**参数**:
- `key`: 键名
- `localDevPtr`: 本地NPU内存地址
- `size`: 数据大小
- `stream`: ACL Stream用于异步执行

**返回**: `Status::OK()` 成功, 否则返回错误信息

**注意**:
- 此操作立即返回，传输在后台进行
- 必须通过`aclrtSynchronizeStream`等待完成
- stream必须在同一Device上创建

**示例**:
```cpp
aclrtStream stream;
aclrtCreateStream(&stream);

float* localDevPtr;
aclrtMalloc((void**)&localDevPtr, 1024 * sizeof(float), ACL_MEM_MALLOC_HUGE_FIRST);

auto status = client.GetAsync("layer1_weights", localDevPtr,
                              1024 * sizeof(float), stream);

// 执行其他操作
DoOtherWork();

// 等待传输完成
aclrtSynchronizeStream(stream);

// 现在可以安全使用数据
```

#### Disconnect

断开与服务器的连接。

```cpp
Status Disconnect();
```

**返回**: `Status::OK()` 成功, 否则返回错误信息

**示例**:
```cpp
client.Disconnect();
```

---

### PerformanceMonitor

性能监控类。

#### 头文件

```cpp
#include "zerokv/zerokv_monitor.h"
```

#### GetStats

获取指定操作类型的统计信息。

```cpp
AggregatedStats GetStats(OperationType type) const;
```

**参数**:
- `type`: 操作类型 (PUT, GET, DELETE, P2P_SEND, P2P_RECV)

**返回**: 聚合统计信息

**示例**:
```cpp
auto stats = monitor->GetStats(OperationType::GET);
std::cout << "Total Ops: " << stats.totalOps << "\n";
std::cout << "Success Rate: "
          << (100.0 * stats.successOps / stats.totalOps) << "%\n";
std::cout << "Avg Latency: " << stats.avgLatencyUs << " us\n";
std::cout << "P95 Latency: " << stats.p95LatencyUs << " us\n";
std::cout << "Throughput: " << stats.throughputMBps << " MB/s\n";
```

#### ExportPrometheus

导出Prometheus格式的指标。

```cpp
std::string ExportPrometheus() const;
```

**返回**: Prometheus格式的指标字符串

**示例**:
```cpp
std::string metrics = monitor->ExportPrometheus();
std::cout << metrics;
```

#### StartRealTimeDisplay

启动实时性能显示 (终端输出)。

```cpp
void StartRealTimeDisplay();
```

**示例**:
```cpp
monitor->StartRealTimeDisplay();
// 每秒在终端输出实时统计
```

#### StopRealTimeDisplay

停止实时性能显示。

```cpp
void StopRealTimeDisplay();
```

---

### Status

状态返回类型。

#### 头文件

```cpp
#include "datasystem/utils/status.h"
```

#### 成员函数

```cpp
bool ok() const;                    // 是否成功
std::string message() const;         // 错误信息
StatusCode code() const;            // 错误码
```

#### 静态方法

```cpp
static Status OK();                              // 成功状态
static Status Error(const std::string& msg);     // 错误状态
```

**示例**:
```cpp
Status DoSomething() {
    if (error_condition) {
        return Status::Error("Something went wrong");
    }
    return Status::OK();
}

auto status = DoSomething();
if (!status.ok()) {
    LOG(ERROR) << "Error: " << status.message();
}
```

---

## Python API

### Server

服务端类。

#### 导入

```python
import zerokv
```

#### 构造函数

```python
Server(device_id: int)
```

**参数**:
- `device_id`: NPU设备ID

**示例**:
```python
server = zerokv.Server(device_id=0)
```

#### start

启动服务器。

```python
start(ip: str, port: int) -> None
```

**参数**:
- `ip`: 监听IP地址
- `port`: 监听端口

**异常**: 启动失败时抛出`RuntimeError`

**示例**:
```python
try:
    server.start("0.0.0.0", 50051)
    print("Server started successfully")
except RuntimeError as e:
    print(f"Failed to start server: {e}")
```

#### put

注册NumPy数组或PyTorch Tensor到KV存储。

```python
put(key: str, data: Union[np.ndarray, torch.Tensor]) -> None
```

**参数**:
- `key`: 键名
- `data`: NumPy数组或PyTorch Tensor

**异常**: 注册失败时抛出`RuntimeError`

**注意**:
- 自动检测数据类型 (支持float32, float16, int32, int64)
- PyTorch Tensor必须在NPU设备上
- NumPy数组会自动拷贝到NPU

**示例**:
```python
import numpy as np
import torch

# NumPy数组
data = np.random.randn(1024, 1024).astype(np.float32)
server.put("numpy_data", data)

# PyTorch Tensor
tensor = torch.randn(1024, 1024, device='npu:0')
server.put("torch_data", tensor)
```

#### delete

删除键。

```python
delete(key: str) -> None
```

**参数**:
- `key`: 键名

**异常**: 删除失败时抛出`RuntimeError`

**示例**:
```python
server.delete("numpy_data")
```

#### get_monitor

获取性能监控对象。

```python
get_monitor() -> Monitor
```

**返回**: 监控对象

**示例**:
```python
monitor = server.get_monitor()
```

#### shutdown

关闭服务器。

```python
shutdown() -> None
```

**示例**:
```python
server.shutdown()
```

---

### Client

客户端类。

#### 构造函数

```python
Client(device_id: int)
```

**参数**:
- `device_id`: 本地NPU设备ID

**示例**:
```python
client = zerokv.Client(device_id=1)
```

#### connect

连接到服务器。

```python
connect(server_ip: str, port: int) -> None
```

**参数**:
- `server_ip`: 服务器IP地址
- `port`: 服务器端口

**异常**: 连接失败时抛出`RuntimeError`

**示例**:
```python
client.connect("192.168.1.100", 50051)
```

#### get

获取数据为NumPy数组。

```python
get(key: str) -> np.ndarray
```

**参数**:
- `key`: 键名

**返回**: NumPy数组 (在CPU内存中)

**异常**: 获取失败时抛出`RuntimeError`

**注意**: 数据会从NPU拷贝到CPU

**示例**:
```python
data = client.get("numpy_data")
print(data.shape)  # (1024, 1024)
print(data.dtype)  # float32
```

#### get_torch

获取数据为PyTorch Tensor。

```python
get_torch(key: str) -> torch.Tensor
```

**参数**:
- `key`: 键名

**返回**: PyTorch Tensor (在NPU设备上)

**异常**: 获取失败时抛出`RuntimeError`

**示例**:
```python
tensor = client.get_torch("torch_data")
print(tensor.device)  # npu:1
print(tensor.shape)   # torch.Size([1024, 1024])
```

#### disconnect

断开连接。

```python
disconnect() -> None
```

**示例**:
```python
client.disconnect()
```

---

### Monitor

性能监控类。

#### get_stats

获取指定操作的统计信息。

```python
get_stats(operation: str) -> Dict[str, float]
```

**参数**:
- `operation`: 操作类型 ('PUT', 'GET', 'DELETE')

**返回**: 统计信息字典

**示例**:
```python
stats = monitor.get_stats('GET')
print(f"Total Ops: {stats['total_ops']}")
print(f"Success Ops: {stats['success_ops']}")
print(f"Avg Latency: {stats['avg_latency_us']} us")
print(f"P95 Latency: {stats['p95_latency_us']} us")
print(f"Throughput: {stats['throughput_mbps']} MB/s")
```

#### get_all_stats

获取所有操作的统计信息。

```python
get_all_stats() -> Dict[str, Dict[str, float]]
```

**返回**: 所有操作的统计信息

**示例**:
```python
all_stats = monitor.get_all_stats()
for op, stats in all_stats.items():
    print(f"\n{op} Operation:")
    print(f"  P95 Latency: {stats['p95_latency_us']} us")
```

#### start_display

启动实时性能显示。

```python
start_display() -> None
```

**示例**:
```python
monitor.start_display()
# 每秒在终端输出实时统计
```

#### stop_display

停止实时性能显示。

```python
stop_display() -> None
```

---

## 数据类型映射

### C++ 到 Python

| C++ Type | Python Type | NumPy dtype | PyTorch dtype |
|----------|-------------|-------------|---------------|
| float | float | np.float32 | torch.float32 |
| double | float | np.float64 | torch.float64 |
| int32_t | int | np.int32 | torch.int32 |
| int64_t | int | np.int64 | torch.int64 |
| uint8_t | int | np.uint8 | torch.uint8 |

### HcclDataType

| 枚举值 | 含义 | 对应dtype |
|--------|------|-----------|
| HCCL_DATA_TYPE_FP32 | 32位浮点 | np.float32 |
| HCCL_DATA_TYPE_FP16 | 16位浮点 | np.float16 |
| HCCL_DATA_TYPE_INT32 | 32位整数 | np.int32 |
| HCCL_DATA_TYPE_INT64 | 64位整数 | np.int64 |

---

## 完整示例

### C++ 完整示例

```cpp
#include "zerokv/zerokv_server.h"
#include "zerokv/zerokv_client.h"
#include <iostream>
#include <vector>

int main() {
    // 初始化ACL
    aclInit(nullptr);
    aclrtSetDevice(0);

    // 启动Server (Device 0)
    ZeroKVServer server(0);
    auto status = server.Start("0.0.0.0", 50051);
    if (!status.ok()) {
        std::cerr << "Failed to start server: " << status.message() << "\n";
        return 1;
    }

    // 分配NPU内存并填充数据
    size_t dataSize = 1024 * 1024 * sizeof(float);  // 1M floats
    float* devPtr;
    aclrtMalloc((void**)&devPtr, dataSize, ACL_MEM_MALLOC_HUGE_FIRST);

    std::vector<float> hostData(1024 * 1024, 3.14f);
    aclrtMemcpy(devPtr, dataSize, hostData.data(), dataSize,
                ACL_MEMCPY_HOST_TO_DEVICE);

    // 注册到KV存储
    status = server.Put("model_weights", devPtr, dataSize);
    if (!status.ok()) {
        std::cerr << "Failed to put: " << status.message() << "\n";
        return 1;
    }

    // Client连接并获取 (Device 1)
    aclrtSetDevice(1);
    ZeroKVClient client(1);
    status = client.Connect("127.0.0.1", 50051);
    if (!status.ok()) {
        std::cerr << "Failed to connect: " << status.message() << "\n";
        return 1;
    }

    // 分配本地内存
    float* localDevPtr;
    aclrtMalloc((void**)&localDevPtr, dataSize, ACL_MEM_MALLOC_HUGE_FIRST);

    // 获取数据
    status = client.Get("model_weights", localDevPtr, dataSize);
    if (!status.ok()) {
        std::cerr << "Failed to get: " << status.message() << "\n";
        return 1;
    }

    // 验证数据
    std::vector<float> resultData(1024 * 1024);
    aclrtMemcpy(resultData.data(), dataSize, localDevPtr, dataSize,
                ACL_MEMCPY_DEVICE_TO_HOST);

    bool success = (resultData == hostData);
    std::cout << "Data verification: " << (success ? "PASS" : "FAIL") << "\n";

    // 查看性能统计
    auto monitor = client.GetMonitor();
    auto stats = monitor->GetStats(OperationType::GET);
    std::cout << "Get P95 Latency: " << stats.p95LatencyUs << " us\n";
    std::cout << "Throughput: " << stats.throughputMBps << " MB/s\n";

    // 清理
    client.Disconnect();
    server.Shutdown();
    aclrtFree(devPtr);
    aclrtFree(localDevPtr);
    aclFinalize();

    return 0;
}
```

### Python 完整示例

```python
import zerokv
import numpy as np
import torch

def main():
    # 启动Server
    server = zerokv.Server(device_id=0)
    server.start("0.0.0.0", 50051)
    print("Server started")

    # 注册NumPy数据
    data = np.random.randn(1024, 1024).astype(np.float32)
    server.put("embeddings", data)
    print("Put embeddings")

    # 注册PyTorch Tensor
    torch.npu.set_device(0)
    tensor = torch.randn(512, 512, device='npu:0')
    server.put("model_state", tensor)
    print("Put model_state")

    # Client连接
    torch.npu.set_device(1)
    client = zerokv.Client(device_id=1)
    client.connect("127.0.0.1", 50051)
    print("Client connected")

    # 获取NumPy数据
    result_np = client.get("embeddings")
    print(f"Got embeddings: shape={result_np.shape}, dtype={result_np.dtype}")

    # 获取PyTorch Tensor
    result_torch = client.get_torch("model_state")
    print(f"Got model_state: shape={result_torch.shape}, device={result_torch.device}")

    # 查看性能统计
    monitor = client.get_monitor()
    stats = monitor.get_stats('GET')
    print(f"\nPerformance Stats:")
    print(f"  Total Ops: {stats['total_ops']}")
    print(f"  Avg Latency: {stats['avg_latency_us']:.2f} us")
    print(f"  P95 Latency: {stats['p95_latency_us']:.2f} us")
    print(f"  Throughput: {stats['throughput_mbps']:.2f} MB/s")

    # 清理
    client.disconnect()
    server.shutdown()
    print("Shutdown complete")

if __name__ == "__main__":
    main()
```

---

## 错误处理

### C++ 错误码

通过`Status::code()`获取:

| 错误码 | 含义 |
|--------|------|
| OK | 成功 |
| INVALID_ARGUMENT | 参数无效 |
| NOT_FOUND | 键不存在 |
| ALREADY_EXISTS | 键已存在 |
| RESOURCE_EXHAUSTED | 资源耗尽 |
| UNAVAILABLE | 服务不可用 |
| DEADLINE_EXCEEDED | 超时 |
| INTERNAL | 内部错误 |

### Python 异常

| 异常类型 | 触发条件 |
|----------|---------|
| RuntimeError | 一般性错误 |
| ValueError | 参数错误 |
| ConnectionError | 网络连接错误 |
| TimeoutError | 操作超时 |

**示例**:
```python
try:
    client.connect("192.168.1.100", 50051)
except ConnectionError as e:
    print(f"Connection failed: {e}")
except TimeoutError as e:
    print(f"Connection timeout: {e}")
```

---

## 性能优化建议

1. **预分配内存**: 避免频繁分配/释放NPU内存
2. **使用异步API**: 对于大数据传输使用`GetAsync`
3. **批量操作**: 合并多个小请求
4. **连接复用**: 保持长连接，避免频繁连接/断开
5. **内存对齐**: 确保数据512字节对齐

**示例**:
```cpp
// 预分配缓冲区
std::vector<float*> bufferPool(10);
for (auto& buf : bufferPool) {
    aclrtMalloc((void**)&buf, dataSize, ACL_MEM_MALLOC_HUGE_FIRST);
}

// 复用缓冲区
for (int i = 0; i < 100; ++i) {
    int bufIdx = i % bufferPool.size();
    client.Get(keys[i], bufferPool[bufIdx], dataSize);
}
```
