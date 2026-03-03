# ZeroKV 第二次会议纪要

**日期**: 2026-03-03
**阶段**: 方案设计
**参与者**: PM, Arch, Dev, QA, Coord

---

## 1. 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                        ZeroKV 架构                            │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐     │
│  │  Python API │    │  C++ API    │    │  HCCL/NCCL  │     │
│  └──────┬──────┘    └──────┬──────┘    └──────┬──────┘     │
│         │                  │                  │             │
│         └──────────────────┼──────────────────┘             │
│                            ▼                                  │
│               ┌───────────────────────┐                      │
│               │     Client SDK        │                      │
│               └───────────┬───────────┘                      │
│                           ▼                                   │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │                  Server 节点                            │ │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐    │ │
│  │  │  Request    │  │   Storage   │  │   UCX       │    │ │
│  │  │  Handler    │──│   Engine    │──│   Transport │    │ │
│  │  └─────────────┘  └─────────────┘  └─────────────┘    │ │
│  └─────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. 架构决策

| 决策项 | 方案 | 备注 |
|--------|------|------|
| 存储引擎 | 自研 | 内存池+分块，大value优化 |
| 通信协议 | 自研RPC | 基于UCX |
| 用户内存 | RDMA Write | zero-copy到用户内存 |
| 元数据 | Redis/Etcd | 路由信息存储 |
| HCCL/NCCL | 封装层 | 提供集合通信接口 |

---

## 3. API设计

### C++ API
```cpp
class ZeroKVClient {
public:
    Connect(const std::vector<std::string>& servers);
    Disconnect();

    // 基本操作
    Put(const std::string& key, const void* value, size_t size);
    Get(const std::string& key, void* buffer, size_t* size);
    Delete(const std::string& key);

    // 用户内存场景
    PutWithUserMem(const std::string& key,
                   void* user_addr, uint32_t rkey, size_t size);
    GetToUserMem(const std::string& key,
                 void* user_addr, uint32_t rkey, size_t size);
};
```

### Python API
```python
class ZeroKV:
    def connect(self, servers: List[str]):
        pass

    def put(self, key: str, value: bytes):
        pass

    def get(self, key: str) -> bytes:
        pass

    def get_to_buffer(self, key: str, buffer):
        pass
```

---

## 4. 待讨论（细节阶段）

- [ ] 存储引擎详细设计
- [ ] 协议格式定义
- [ ] 错误处理策略
- [ ] 项目目录结构

---

## 5. 下次会议

- **主题**: 细节讨论
- **内容**: 存储引擎、协议、错误处理、目录结构
