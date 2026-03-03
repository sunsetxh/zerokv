# ZeroKV 第四次迭代会议纪要

**日期**: 2026-03-03
**阶段**: 代码实现
**参与者**: Dev, Coord

---

## 1. MVP实现完成

### 已完成模块

| 模块 | 文件 | 状态 |
|------|------|------|
| 公共类型 | include/zerokv/common.h | ✅ |
| 存储引擎 | src/storage/storage.cc | ✅ |
| 协议编解码 | src/protocol/codec.cc | ✅ |
| UCX传输层 | src/transport/ucx_transport.cc | ✅ |
| Client API | src/client/client.cc | ✅ |
| Server框架 | src/server/server.cc | ✅ |
| Python绑定 | python/pybind.cc | ✅ |
| Benchmark | tools/benchmark/benchmark.cc | ✅ |
| 元数据(Redis/Etcd) | src/metadata/metadata.cc | ✅ (placeholder) |

### 项目结构

```
zerokv/
├── CMakeLists.txt
├── include/zerokv/
│   ├── common.h         # OpCode, Status, MemoryType等
│   ├── client.h         # Client API
│   ├── storage.h        # StorageEngine
│   ├── protocol.h       # ProtocolCodec
│   ├── transport.h     # Transport抽象类
│   └── metadata.h      # MetadataStore接口
├── src/
│   ├── client/client.cc
│   ├── server/
│   │   ├── server.cc
│   │   └── main.cc     # 服务端入口
│   ├── storage/storage.cc
│   ├── protocol/codec.cc
│   ├── transport/
│   │   ├── ucx_transport.h
│   │   └── ucx_transport.cc  # UCX实现
│   └── metadata/metadata.cc   # Redis/Etcd占位
├── python/
│   ├── pybind.cc
│   ├── zerokv/__init__.py
│   └── setup.py
├── tests/unit/
│   └── test_storage.cc
└── tools/benchmark/
    ├── CMakeLists.txt
    └── benchmark.cc
```

---

## 2. 新增功能

### 2.1 UCX传输层完善
- 地址解析和监听
- 端点创建和管理
- RDMA put/get支持
- 用户内存零拷贝接口

### 2.2 元数据管理
- RedisMetadataStore
- EtcdMetadataStore
- Key路由管理
- 节点注册/注销

### 2.3 Server可执行文件
- zerokv_server 命令行工具
- 支持 -a/-p/-m 参数

---

## 3. 待完善

- [ ] UCX连接细节优化
- [ ] Redis/Etcd客户端集成 (hiredis / etcd-client)
- [ ] HCCL传输层
- [ ] NCCL传输层
- [ ] 集成测试

---

## 4. 编译说明

```bash
mkdir build && cd build
cmake .. -DBUILD_PYTHON=ON -DBUILD_TESTS=ON -DBUILD_BENCHMARK=ON
make -j$(nproc)

# 运行server
./zerokv_server -a 0.0.0.0 -p 5000 -m 4096
```
