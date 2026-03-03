# ZeroKV 第三次会议纪要

**日期**: 2026-03-03
**阶段**: 细节讨论
**参与者**: PM, Arch, Dev, QA, Coord

---

## 1. 存储引擎设计

采用**分层存储**架构：

```
┌─────────────────────────────────────────┐
│           Storage Engine                 │
├─────────────────────────────────────────┤
│  Index Layer (B+Tree / Hash)            │
│  - key → value_handle映射               │
├─────────────────────────────────────────┤
│  Memory Pool                            │
│  - 预分配内存池                         │
│  - Chunk 64KB                           │
├─────────────────────────────────────────┤
│  Value Storage                          │
│  - 小value (<1MB): 直接存储             │
│  - 大value (>=1MB): 分块存储            │
└─────────────────────────────────────────┘
```

---

## 2. 协议格式定义

基于UCX的RPC协议（16字节Header）：

```c
// Request Header
struct RequestHeader {
    uint8_t  opcode;      // 0=Put, 1=Get, 2=Delete, 3=GetToUserMem
    uint8_t  flags;
    uint16_t key_len;
    uint32_t value_len;
    uint64_t request_id;
    uint32_t user_rkey;   // 用户内存rkey
    uint64_t user_addr;   // 用户内存地址
};

// Response Header
struct ResponseHeader {
    uint8_t  status;      // 0=OK, 1=NotFound, 2=Error
    uint8_t  flags;
    uint16_t reserved;
    uint32_t value_len;
    uint64_t request_id;
};
```

---

## 3. 错误处理策略

| 错误类型 | 处理策略 |
|----------|----------|
| 网络超时 | 重试3次，指数退避 |
| 节点故障 | 路由更新，重新选择节点 |
| 内存不足 | 返回错误，拒绝写入 |
| 用户内存无效 | 返回错误，包含错误码 |

---

## 4. 项目目录结构

```
zerokv/
├── docs/                    # 文档
├── include/zerokv/          # 头文件
├── src/                    # 源代码
│   ├── client/            # 客户端
│   ├── server/            # 服务端
│   ├── storage/          # 存储引擎
│   ├── protocol/         # 协议
│   └── transport/        # 传输层 (UCX/HCCL/NCCL)
├── python/                # Python绑定
├── tests/                 # 测试
├── tools/                 # 工具 (benchmark)
└── CMakeLists.txt
```

---

## 5. 传输层设计（修正）

**关键修正**：HCCL/NCCL应该是与UCX平级的传输层

```
┌─────────────────────────────────────────────────────────────┐
│                    ZeroKV Transport Layer                   │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐     │
│  │    UCX     │    │   HCCL     │    │    NCCL     │     │
│  │  (CPU)     │    │ (昇腾GPU)   │    │ (NVIDIA GPU)│     │
│  └─────────────┘    └─────────────┘    └─────────────┘     │
├─────────────────────────────────────────────────────────────┤
│                      RDMA / NVLink / HCCS                   │
└─────────────────────────────────────────────────────────────┘
```

### 各传输层用途

| 传输层 | 适用场景 | 底层硬件 |
|--------|----------|----------|
| UCX | CPU内存 | RDMA (RoCE) |
| HCCL | 昇腾NPU显存 | HCCS |
| NCCL | NVIDIA GPU显存 | NVLink/RDMA |

### 内存类型抽象

```cpp
enum class MemoryType {
    CPU,
    HUAWEI_NPU,   // 昇腾
    NVIDIA_GPU    // NVIDIA
};
```

---

## 6. 待办事项

- [ ] MVP实现顺序确认
- [ ] 性能指标定义
- [ ] 测试环境准备
