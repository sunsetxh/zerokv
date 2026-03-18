# HCCL Plugin 接口兼容性分析

> 角色: 开发工程师 (Dev)
> 日期: 2026-03-04
> 关联: risk-assessment-ascend.md

---

## 一、HCCL API 映射

### 数据类型映射（完整覆盖）

| AXON DataType | HCCL HcclDataType | 支持 |
|--------------|-------------------|------|
| kFloat16 | HCCL_DATA_TYPE_FP16 | 支持 |
| kFloat32 | HCCL_DATA_TYPE_FP32 | 支持 |
| kFloat64 | HCCL_DATA_TYPE_FP64 | 支持 |
| kBFloat16 | HCCL_DATA_TYPE_BFP16 | 支持 |
| kInt8 | HCCL_DATA_TYPE_INT8 | 支持 |
| kInt32 | HCCL_DATA_TYPE_INT32 | 支持 |
| kInt64 | HCCL_DATA_TYPE_INT64 | 支持 |
| kUint8 | HCCL_DATA_TYPE_UINT8 | 支持 |

### ReduceOp 映射

| AXON ReduceOp | HCCL HcclReduceOp | 支持 |
|--------------|-------------------|------|
| kSum | HCCL_REDUCE_SUM | 支持 |
| kProd | HCCL_REDUCE_PROD | 支持 |
| kMax | HCCL_REDUCE_MAX | 支持 |
| kMin | HCCL_REDUCE_MIN | 支持 |
| kAvg | -- | **不支持**（SUM + scale 模拟） |

---

## 二、接口 Gap 分析

| Gap | 严重度 | 描述 | 方案 |
|-----|--------|------|------|
| GAP-1 | 中 | Broadcast in-place 语义差异 | Plugin层条件处理 |
| GAP-2 | 中 | 无 GroupStart/GroupEnd | 返回 no-op |
| GAP-3 | 低 | kAvg 不支持 | SUM + scale |
| GAP-4 | 低 | sendBuf 非 const | const_cast |
| GAP-5 | 中 | 缺少 batch_send_recv | **建议新增**可选接口 |
| GAP-6 | 中 | create_communicator 缺 device_id | **建议扩展**参数 |
| GAP-7 | 低 | HcclCommConfig 未暴露 | 后续迭代 |

**结论：无需阻塞性修改即可完成基本功能对接。**

---

## 三、建议的 plugin.h 增强

### 新增 batch_send_recv()

```cpp
struct SendRecvItem {
    void*    buf;
    size_t   count;
    DataType dtype;
    int      peer;
    bool     is_send;
};

virtual Status batch_send_recv(Communicator::Ptr comm,
                               const std::vector<SendRecvItem>& items,
                               void* stream) {
    // Default: sequential fallback
    for (const auto& item : items) {
        Status s = item.is_send
            ? send(comm, item.buf, item.count, item.dtype, item.peer, stream)
            : recv(comm, item.buf, item.count, item.dtype, item.peer, stream);
        if (!s.ok()) return s;
    }
    return Status::OK();
}
```

### create_communicator 扩展

```cpp
struct CommOptions {
    int device_id = -1;          // -1 = auto
    size_t buffer_size = 0;      // 0 = default
    bool deterministic = false;
};
```

---

## 四、产出文件

- `src/plugin/hccl_plugin.cpp` — 骨架实现（可编译 skeleton）
- `CMakeLists.txt` — 新增 `AXON_BUILD_HCCL` 构建选项

---

**参考资料：**
- [HCCL hccl.h header](https://gitee.com/ascend/pytorch/blob/83742e7653ebd4747b61a1880f5115f85ed3c933/third_party/hccl/inc/hccl/hccl.h)
- [HCCL hccl_types.h](https://gitee.com/mindspore/graphengine/blob/704a9eb441a711cd9f83e6462d283a1d049e6c6f/inc/external/hccl/hccl_types.h)
- [HCCL AXON test](https://github.com/zzudongxiang/ascend.cl/blob/master/hccl/hccl_axon_rootinfo_test.cc)
- [HCCL API Reference](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/hcclapiref/hcclcpp_07_0011.html)
