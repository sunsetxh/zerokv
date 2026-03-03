# ZeroKV 开发指南

## 项目结构

```
zerokv/
├── include/zerokv/     # 头文件
├── src/               # 源代码
├── python/            # Python绑定
├── tests/             # 测试用例
└── .github/          # GitHub配置
```

## 任务管理

### 优先级定义

| 优先级 | 标签 | 描述 |
|--------|------|------|
| P1 | `P1` | 必须完成 - 影响核心功能 |
| P2 | `P2` | 应当完成 - 重要但非紧急 |
| P3 | `P3` | 可以完成 - 改进性需求 |

### 任务状态

| 状态 | 标签 | 描述 |
|------|------|------|
| 待处理 | `backlog` | 等待开始 |
| 进行中 | `in-progress` | 正在开发 |
| 阻塞 | `blocked` | 被其他任务阻塞 |
| 已完成 | `completed` | 已完成开发 |

### 工作流程

1. **领取任务**: 从 GitHub Issues 列表中选择 P1 优先级的任务
2. **开始工作**: 将 Issue 状态改为 `in-progress`
3. **开发**: 在本地分支进行开发
4. **提交**: 创建 Pull Request，关联相关 Issue
5. **完成**: PR 合并后，将 Issue 标记为 `completed`

### 里程碑

当前里程碑: **v0.2.0 - Core Features**

包含:
- UCX 连接优化
- Redis/Etcd 客户端集成
- HCCL 传输层
- NCCL 传输层
- 集成测试完善

## 编译

```bash
mkdir build && cd build
cmake .. -DBUILD_TESTS=ON
make -j$(nproc)
```

## 运行测试

```bash
./bin/tests/test_storage
```

### 本地测试

```bash
# 运行所有简单测试
./test_storage_standalone
./test_cache
./test_features
./test_batch
./test_integration
```

## 使用示例

### C++

```cpp
#include "zerokv/client.h"

zerokv::Client client;
client.connect({"localhost:5000"});
client.put("key", "value");
```

### Python

```python
from zerokv import ZeroKV
client = ZeroKV()
client.connect(["localhost:5000"])
client.put("key", "value")
```

## 周会流程

每周一进行任务进度Review:

1. 检查 P1 任务状态
2. 更新里程碑进度
3. 识别阻塞问题
4. 重新分配资源
