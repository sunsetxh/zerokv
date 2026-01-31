# ZeroKV - 零拷贝NPU显存KV中间件

## 项目信息

- **项目名称**: ZeroKV
- **版本**: 1.0.0
- **创建日期**: 2025-01-31
- **许可证**: Apache License 2.0

## 核心特性

🚀 **零拷贝传输** - 基于P2P-Transfer的NPU间直接内存传输，无需经过Host DDR
⚡ **高性能** - 延迟 <50μs, 带宽 >180GB/s，QPS >5M ops/s
🏗️ **双平面架构** - UCX控制平面处理元数据，P2P数据平面处理零拷贝传输
🐍 **多语言支持** - C++核心库 + Python绑定 + PyTorch集成
📊 **完整监控** - Prometheus + Grafana + Alertmanager全栈监控方案
🧪 **Mock模式** - 基于UCX的P2P Mock实现，无需NPU硬件即可开发测试

## 技术栈

- **控制平面**: UCX (Unified Communication X)
- **数据平面**: P2P-Transfer (HCCS/RoCE)
- **序列化**: Protocol Buffers
- **监控**: Prometheus + Grafana
- **语言绑定**: pybind11
- **构建系统**: CMake
- **测试框架**: Google Test

## 目标性能

| 指标 | 目标值 | 说明 |
|------|-------|------|
| Get延迟 (P50) | <30μs | 1MB数据, 同节点 |
| Get延迟 (P95) | <50μs | 1MB数据, 同节点 |
| Get延迟 (P99) | <100μs | 1MB数据, 同节点 |
| Put延迟 | <10μs | 仅元数据注册 |
| QPS | >5M ops/s | 1KB小对象 |
| 带宽 (HCCS) | >180GB/s | 大块数据传输 |
| 带宽 (RoCE) | >100GB/s | 跨节点传输 |
| 并发连接 | >1000 | 单Server |

## 架构设计

```
┌──────────────────────────────────────────────────────────┐
│                   Application Layer                      │
│              (C++ API / Python API)                      │
└────────────────────┬─────────────────────────────────────┘
                     │
        ┌────────────┴────────────┐
        │                         │
┌───────▼────────┐        ┌──────▼──────┐
│  ZeroKVClient  │        │ ZeroKVServer│
│   (Device 1)   │        │ (Device 0)  │
└───────┬────────┘        └──────┬──────┘
        │                         │
        │   ┌─────────────────────┤
        │   │   Control Plane     │
        │   │   (UCX RPC)         │
        │   │   - Metadata        │
        │   └─────────────────────┤
        │                         │
        │   ┌─────────────────────┤
        │   │   Data Plane        │
        └───┤   (P2P-Transfer)    │
            │   - Zero-copy       │
            └─────────────────────┘
                     │
        ┌────────────┴────────────┐
        │                         │
┌───────▼────────┐        ┌──────▼──────┐
│   NPU Memory   │◄──────►│ NPU Memory  │
│   (Device 1)   │        │ (Device 0)  │
└────────────────┘        └─────────────┘
```

## 开发计划

总计: **12周** (6个里程碑)

### 第1-2周: 基础设施
- P2P Mock实现 (基于UCX)
- UCX控制平面 (Server/Client)
- Protobuf消息定义
- CI/CD配置

### 第3-5周: 核心功能
- ZeroKVServer实现 (Put/Delete/管理)
- ZeroKVClient实现 (Get/BatchGet)
- 错误处理和重试机制
- 优雅关闭

### 第6-7周: Python绑定
- pybind11集成
- NumPy数组支持
- PyTorch Tensor支持
- Python文档和示例

### 第8-9周: 性能优化
- 零拷贝路径优化
- 连接池优化
- 批量操作
- 性能基准测试

### 第10周: 监控系统
- PerformanceMonitor实现
- Prometheus Exporter
- Grafana Dashboard
- 告警规则

### 第11-12周: 测试与发布
- 单元测试 (80%覆盖率)
- 集成测试
- 压力测试
- 文档完善
- v1.0.0发布

## 使用场景

1. **分布式训练** - 在多NPU训练中共享模型参数、梯度
2. **模型服务** - 多个推理实例共享模型权重
3. **特征缓存** - 缓存embedding等中间结果
4. **检查点管理** - 高效存储和恢复训练检查点
5. **数据预处理** - 共享预处理后的数据集

## 快速开始

```bash
# 1. 构建项目
cd zerokv
./scripts/build.sh --release

# 2. 安装Python包
cd python && pip install -e .

# 3. 启动监控
cd monitoring && docker-compose up -d

# 4. 运行示例
# 终端1: 启动Server
./build/bin/simple_server

# 终端2: 运行Client
./build/bin/simple_client

# 或者运行Python示例
python examples/python_example.py server
python examples/python_example.py client
```

## 贡献指南

详见 [CONTRIBUTING.md](CONTRIBUTING.md)

## 联系方式

- GitHub Issues: 提交Bug和功能请求
- Email: zerokv-dev@example.com

---

**ZeroKV - 让NPU显存管理零拷贝、高性能、易用！**
