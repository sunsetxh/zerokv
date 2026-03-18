# Ascend NPU + HCCL 集成可行性调研

> 角色: 技术架构师 (Arch)
> 日期: 2026-03-04
> 关联: risk-assessment-ascend.md

---

## 一、HCCL C API 调研

### 1.1 HCCL 提供独立的 C/C++ API

HCCL 作为 CANN SDK 的核心组件独立存在，提供纯 C 接口，不依赖 MindSpore 或 PyTorch。

### 1.2 头文件与库文件

| 文件 | 路径 | 说明 |
|------|------|------|
| `hccl/hccl.h` | `${CANN_PATH}/include/hccl/hccl.h` | 主 API 头文件 |
| `hccl/hccl_types.h` | `${CANN_PATH}/include/hccl/hccl_types.h` | 类型定义 |
| `libhccl.so` | `${CANN_PATH}/lib64/libhccl.so` | 主库 |
| `libascendcl.so` | `${CANN_PATH}/lib64/libascendcl.so` | ACL 运行时 |

### 1.3 核心 API

**通信域管理：**
```c
HcclResult HcclGetRootInfo(HcclRootInfo *rootInfo);
HcclResult HcclCommInitRootInfo(uint32_t nRanks, const HcclRootInfo *rootInfo,
                                 uint32_t rank, HcclComm *comm);
HcclResult HcclCommDestroy(HcclComm comm);
```

**集合通信：**
```c
HcclResult HcclAllReduce(void *sendBuf, void *recvBuf, uint64_t count,
                          HcclDataType dataType, HcclReduceOp op,
                          HcclComm comm, aclrtStream stream);
HcclResult HcclBroadcast(void *buf, uint64_t count, HcclDataType dataType,
                          uint32_t root, HcclComm comm, aclrtStream stream);
HcclResult HcclAllGather(...);
HcclResult HcclReduceScatter(...);
```

**AXON 点对点通信（关键发现）：**
```c
HcclResult HcclSend(void *sendBuf, uint64_t count, HcclDataType dataType,
                     uint32_t destRank, HcclComm comm, aclrtStream stream);
HcclResult HcclRecv(void *recvBuf, uint64_t count, HcclDataType dataType,
                     uint32_t srcRank, HcclComm comm, aclrtStream stream);
HcclResult HcclBatchSendRecv(HcclSendRecvItemDef *items, uint32_t itemNum,
                              HcclComm comm, aclrtStream stream);
```

### 1.4 Stream 机制

HCCL 使用 `aclrtStream`，与 CUDA `cudaStream_t` 模型完全一致。

---

## 二、Ascend NPU 内存管理

### 2.1 内存 API

```c
aclError aclrtMalloc(void **devPtr, size_t size, aclrtMemMallocPolicy policy);
aclError aclrtFree(void *devPtr);
aclError aclrtMallocHost(void **hostPtr, size_t size);  // pinned memory
aclError aclrtMemcpy(void *dst, size_t dstMax, const void *src,
                      size_t count, aclrtMemcpyKind kind);
```

### 2.2 互连带宽

| 互连层级 | 技术 | 带宽 |
|---------|------|------|
| 同节点 NPU-NPU | HCCS | 30 GB/s x 3 = 90 GB/s |
| 跨节点 NPU-NPU | RoCE v2 RDMA | 200-400 Gbps |
| NPU-Host | PCIe Gen4 x24 | ~30 GB/s |

### 2.3 IPC 机制

CANN 提供类似 CUDA VMM 的虚拟内存管理 API：
- `aclrtMallocPhysical` / `aclrtReserveMemAddress` / `aclrtMapMem`
- `aclrtMemExportToShareableHandleV2` / `aclrtMemImportFromShareableHandleV2`

---

## 三、方案对比

| 方案 | 跨节点带宽 | 同节点带宽 | 延迟 | 推荐 |
|------|-----------|-----------|------|------|
| **A: HCCL Plugin直连** | ~25 GB/s | ~90 GB/s | ~1-2us | **首选** |
| B: NPU→Host→UCX→Host→NPU | ~10 GB/s | ~30 GB/s | ~15-25us | Fallback |
| C: UCX Ascend Transport | 不存在 | N/A | N/A | 排除 |

---

## 四、结论

风险实际严重程度为**中低**。HCCL 提供完整的 C API（含 AXON），Plugin 接口 1:1 映射，实现成本约 200-300 行代码。

**后续验证步骤：**
1. Phase 1（1-2周）：获取 Ascend 环境，编写独立 HCCL AXON 测试
2. Phase 2（1-2周）：基于骨架实现 hccl_plugin.cpp
3. Phase 3（2-3周）：性能与稳定性测试

---

**参考资料：**
- [HCCL API - CANN](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/hcclapiref/hcclcpp_07_0011.html)
- [Ascend/pytorch - hccl.h](https://gitee.com/ascend/pytorch/blob/83742e7653ebd4747b61a1880f5115f85ed3c933/third_party/hccl/inc/hccl/hccl.h)
- [HCCL AXON Test](https://github.com/zzudongxiang/ascend.cl/blob/master/hccl/hccl_axon_rootinfo_test.cc)
- [Mooncake Issue #1312 - CANN VMM APIs](https://github.com/kvcache-ai/Mooncake/issues/1312)
- [CloudMatrix384 Architecture](https://arxiv.org/html/2506.12708v2)
