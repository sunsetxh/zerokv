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

### 编译

```bash
# 安装依赖
sudo apt-get install cmake g++ python3-dev

# 克隆代码
cd zerokv
mkdir build && cd build

# 编译C++库
cmake ..
make -j$(nproc)

# 安装Python包
cd ../python
pip install -e .
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

## 开发模式

在没有NPU硬件的环境中，使用UCX Mock模式进行开发：

```cpp
#include "common/p2p_ucx_mock.h"

// 初始化Mock环境
P2PMockInit(true);  // 使用RDMA

// 正常使用API
HcclRootInfo rootInfo;
P2PGetRootInfo(&rootInfo);
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
