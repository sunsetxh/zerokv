# ZeroKV 团队决议报告

**项目**: 高性能分布式KV存储库
**日期**: 2026-03-03
**状态**: 方案设计完成

---

## 一、核心方案

### 1.1 项目定位

| 属性 | 值 |
|------|-----|
| 项目名称 | ZeroKV |
| 目标 | 高性能分布式KV存储 |
| 底层通信 | UCX 1.19.0 + RoCE |
| 扩展能力 | HCCL (昇腾) + NCCL (NVIDIA) |
| 上层接口 | C++ API + Python API |
| Value规格 | 1KB ~ 1GB |
| 使用场景 | AI训练参数共享 |

### 1.2 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                        ZeroKV 架构                           │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐   │
│  │  Python API │    │  C++ API    │    │  HCCL/NCCL  │   │
│  └──────┬──────┘    └──────┬──────┘    └──────┬──────┘   │
│         │                  │                  │            │
│         └──────────────────┼──────────────────┘            │
│                            ▼                                 │
│               ┌───────────────────────┐                      │
│               │     Client SDK        │                      │
│               └───────────┬───────────┘                      │
│                           ▼                                  │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │                    Storage Engine                        │ │
│  │   (内存池 + 分块存储 + LRU淘汰)                          │ │
│  └─────────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────┤
│                  ZeroKV Transport Layer                      │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐   │
│  │    UCX     │    │   HCCL     │    │    NCCL     │   │
│  │  (CPU)     │    │ (昇腾GPU)   │    │ (NVIDIA GPU)│   │
│  └─────────────┘    └─────────────┘    └─────────────┘   │
├─────────────────────────────────────────────────────────────┤
│                      RDMA / NVLink / HCCS                    │
└─────────────────────────────────────────────────────────────┘
```

---

## 二、关键决策

### 2.1 传输层设计

| 传输层 | 适用场景 | 底层硬件 |
|--------|----------|----------|
| UCX | CPU内存 | RDMA (RoCE v2) |
| HCCL | 昇腾NPU显存 | HCCS |
| NCCL | NVIDIA GPU显存 | NVLink/RDMA |

**内存类型枚举**:
```cpp
enum class MemoryType {
    CPU,        // CPU内存 → UCX
    HUAWEI_NPU, // 昇腾NPU → HCCL
    NVIDIA_GPU  // NVIDIA GPU → NCCL
};
```

### 2.2 存储引擎设计

- **内存池**: 预分配，chunk 64KB
- **索引**: B+Tree 或 Hash
- **大value**: 分块存储 (>=1MB)
- **淘汰策略**: LRU

### 2.3 协议设计

**Request Header (16 bytes)**:
```c
struct RequestHeader {
    uint8_t  opcode;      // 0=Put, 1=Get, 2=Delete, 3=GetToUserMem
    uint8_t  flags;
    uint16_t key_len;
    uint32_t value_len;
    uint64_t request_id;
    uint32_t user_rkey;   // 用户内存rkey
    uint64_t user_addr;   // 用户内存地址
};
```

### 2.4 API设计

**C++ API**:
```cpp
class ZeroKVClient {
public:
    Connect(const std::vector<std::string>& servers);
    Disconnect();

    Put(const std::string& key, const void* value, size_t size);
    Get(const std::string& key, void* buffer, size_t* size);
    Delete(const std::string& key);

    // 用户内存场景
    PutWithUserMem(const std::string& key, void* user_addr, uint32_t rkey, size_t size);
    GetToUserMem(const std::string& key, void* user_addr, uint32_t rkey, size_t size);
};
```

**Python API**:
```python
class ZeroKV:
    def connect(self, servers: List[str]): pass
    def put(self, key: str, value: bytes): pass
    def get(self, key: str) -> bytes: pass
    def get_to_buffer(self, key: str, buffer): pass
```

### 2.5 错误处理策略

| 错误类型 | 处理策略 |
|----------|----------|
| 网络超时 | 重试3次，指数退避 |
| 节点故障 | 路由更新 |
| 内存不足 | 返回错误 |
| 用户内存无效 | 返回错误码 |

---

## 三、MVP版本范围

- [x] 基础KV操作 (Put/Get/Delete)
- [x] UCX网络通信层
- [x] 用户内存零拷贝支持
- [x] 存储引擎 (内存池)
- [x] 多节点支持 (元数据对接redis/etcd)
- [ ] 性能基准测试

**不包含**:
- 持久化存储
- 一致性协议 (使用redis/etcd管理元数据)

---

## 四、项目结构

```
zerokv/
├── docs/                    # 会议纪要
│   ├── meeting-001-requirements.md
│   ├── meeting-002-architecture.md
│   └── meeting-003-details.md
├── include/zerokv/          # 头文件
│   ├── client.h
│   ├── storage.h
│   ├── protocol.h
│   └── transport.h
├── src/                     # 源代码
│   ├── client/
│   ├── server/
│   ├── storage/
│   ├── protocol/
│   └── transport/
│       ├── ucx_transport.cc
│       ├── hccl_transport.cc
│       └── nccl_transport.cc
├── python/                  # Python绑定
├── tests/                   # 测试
├── tools/                   # Benchmark工具
└── CMakeLists.txt
```

---

## 五、分工

| 角色 | 职责 |
|------|------|
| Arch | UCX传输层 + 存储引擎 |
| Dev | C++ API + Python绑定 |
| QA | 测试用例 + 性能基准 |

---

## 六、待办事项

- [ ] MVP代码实现
- [ ] 性能指标定义
- [ ] 测试环境准备
- [ ] 集成测试

---

**下次会议**: 代码实现启动会
