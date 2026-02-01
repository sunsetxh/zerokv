# ZeroKV

高性能零拷贝NPU显存键值对管理中间件，支持NPU间直接内存传输。

## 概述

ZeroKV 是一个专为NPU设备设计的分布式键值存储中间件，提供：

- **零拷贝传输**: 基于P2P-Transfer的NPU间直接内存传输
- **双平面架构**: UCX控制平面 + P2P数据平面
- **高性能**: 延迟 <50μs, 带宽 >180GB/s
- **易用性**: 提供C++和Python接口
- **可观测性**: Prometheus + Grafana完整监控方案

## 快速开始

### 系统要求

- **Linux**: Ubuntu 20.04+ / CentOS 8+ (推荐用于生产环境)
- **macOS**: 用于开发（使用 UCX stub 模式）
- **编译器**: GCC 9+ / Clang 12+
- **CMake**: 3.15+
- **构建工具** (Linux): autoconf, libtool, make (用于构建 UCX)

### 编译

ZeroKV 提供三种构建模式：

#### 模式 1: Stub 模式（快速开发，无需 UCX）

适用于 macOS 开发或快速测试，不需要安装 UCX。

```bash
mkdir build && cd build
cmake .. -DUSE_UCX_STUB=ON -DBUILD_PYTHON=OFF
make -j$(nproc)
```

**注意**: Stub 模式仅用于开发，不支持真实的网络通信。

#### 模式 2: 自动构建模式（推荐，Linux 生产环境）

**默认模式**，自动从源码下载并编译 UCX 1.20.0。

```bash
# 安装构建依赖
sudo apt-get install cmake g++ autoconf libtool make

# 编译（首次需要约 3-5 分钟构建 UCX）
mkdir build && cd build
cmake .. -DBUILD_PYTHON=OFF
make -j$(nproc)

# 运行测试
ctest -V
```

UCX 将被安装到 `build/ucx-install` 目录。

#### 模式 3: 自定义 UCX 路径（高级用户）

如果你已经编译好 UCX 或想使用特定版本：

```bash
# 假设 UCX 安装在 /opt/ucx
mkdir build && cd build
cmake .. -DUSE_UCX_STUB=OFF -DUCX_ROOT=/opt/ucx -DBUILD_PYTHON=OFF
make -j$(nproc)
```

**要求**: UCX_ROOT 必须包含：
- `include/ucp/api/ucp.h`
- `lib/libucp.so` 和 `lib/libucs.so`

### Docker 环境（推荐）

使用 Docker 确保一致的构建环境：

```bash
# 启动构建容器
docker run -it --rm \
  -v $(pwd):/workspace \
  -w /workspace \
  ubuntu:24.04 bash

# 容器内安装依赖
apt-get update && apt-get install -y \
  cmake g++ autoconf libtool make

# 编译
mkdir build && cd build
cmake .. -DBUILD_PYTHON=OFF
make -j$(nproc)
ctest -V
```

### C++ 示例

```cpp
#include "zerokv/zerokv_server.h"
#include "zerokv/zerokv_client.h"

// 启动服务器
ZeroKVServer server(0);  // Device 0
server.Start("0.0.0.0", 50051);

float* devPtr;
aclrtMalloc(&devPtr, 1024 * sizeof(float), ACL_MEM_MALLOC_HUGE_FIRST);
server.Put("model_weights", devPtr, 1024 * sizeof(float));

// 客户端获取
ZeroKVClient client(1);  // Device 1
client.Connect("192.168.1.100", 50051);

float* localPtr;
aclrtMalloc(&localPtr, 1024 * sizeof(float), ACL_MEM_MALLOC_HUGE_FIRST);
client.Get("model_weights", localPtr, 1024 * sizeof(float));
```

### Python 示例

```python
import zerokv
import numpy as np

# 启动服务器
server = zerokv.Server(device_id=0)
server.start("0.0.0.0", 50051)

# 存储NPU数据
data = np.random.randn(1024, 1024).astype(np.float32)
server.put("embeddings", data)

# 客户端获取
client = zerokv.Client(device_id=1)
client.connect("192.168.1.100", 50051)
result = client.get("embeddings")
```

## 架构设计

```
┌─────────────┐         UCX/RPC          ┌─────────────┐
│   Client    │◄────────────────────────►│   Server    │
│  (NPU 1)    │                          │  (NPU 0)    │
└──────┬──────┘                          └──────┬──────┘
       │                                        │
       │         P2P-Transfer (HCCS/RoCE)       │
       └────────────────────────────────────────┘
              Zero-copy NPU Memory
```

- **控制平面**: UCX处理元数据和RPC请求
- **数据平面**: P2P-Transfer处理NPU显存直接传输
- **监控**: Prometheus采集指标，Grafana可视化

## 性能监控

```bash
# 启动监控栈
cd monitoring
docker-compose up -d

# 访问 Grafana
http://localhost:3000
# 默认账号: admin/admin
```

监控指标包括：
- QPS (每秒请求数)
- 延迟分布 (P50/P95/P99)
- 带宽利用率
- 错误率

## 开发指南

### UCX 构建模式对比

| 模式 | 适用场景 | UCX 来源 | 网络通信 | 性能 |
|------|---------|---------|---------|------|
| Stub | macOS 开发 / 快速测试 | 内置 stub | ❌ 模拟 | N/A |
| 自动构建 | Linux 生产 / CI/CD | 源码构建 UCX 1.20.0 | ✅ 真实 | 完整 |
| 自定义路径 | 高级用户 / 性能优化 | 用户提供 | ✅ 真实 | 完整 |

### 常见问题

**Q: 为什么 macOS 上默认使用 stub？**
A: UCX 需要 Linux 内核特性（如 RDMA），macOS 无法编译真实 UCX。stub 模式提供 API 兼容层用于开发。

**Q: 首次编译为什么很慢？**
A: 默认会从源码构建 UCX (约 3-5 分钟)。可以使用 `-DUCX_ROOT` 指向已构建的 UCX 加速。

**Q: 如何使用系统已安装的 UCX？**
A: 使用 `-DUCX_ROOT=/usr/local` (或你的 UCX 安装路径)。

**Q: UCX_ROOT 报错怎么办？**
A: 确保路径包含 `include/ucp/api/ucp.h` 和 `lib/libucp.so`。如果无效，CMake 会明确报错。

### Mock 开发模式

在没有 NPU 硬件的环境中，P2P Mock 层模拟 NPU 内存操作：

```cpp
#include "common/p2p_ucx_mock.h"

// 初始化 Mock 环境
P2PMockInit(true);  // 使用 RDMA 模式

// 正常使用 API
HcclRootInfo rootInfo;
P2PGetRootInfo(&rootInfo);

// Mock 设备内存分配
void* devPtr = nullptr;
MockDeviceMalloc(&devPtr, 1024, 0);
```

## 文档

- [技术设计文档](docs/technical_design_document.md)
- [API参考](docs/api_reference.md)
- [开发排期](docs/development_schedule.md)

## 性能指标

| 操作 | 延迟 (P95) | 吞吐量 |
|------|-----------|--------|
| Put  | <50μs     | >5M ops/s |
| Get  | <50μs     | >180GB/s |
| P2P Send | <30μs  | >200GB/s |

## License

Apache License 2.0

## 贡献

欢迎提交Issue和Pull Request。详见 [CONTRIBUTING.md](CONTRIBUTING.md)

## 联系方式

- Issue Tracker: [GitHub Issues]
- 邮件: zerokv-dev@example.com
