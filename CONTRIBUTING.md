# Contributing to ZeroKV

感谢您对ZeroKV项目的关注！本文档提供了贡献指南。

## 开发环境设置

### 依赖项

**必需:**
- CMake >= 3.15
- C++17 编译器 (GCC 7+, Clang 5+)
- UCX (Unified Communication X)
- Protobuf
- Python 3.7+ (用于Python绑定)

**可选:**
- Ascend NPU 驱动和ACL (用于真实NPU硬件)
- Google Test (用于单元测试)
- pybind11 (用于Python绑定)
- PyTorch (用于PyTorch Tensor支持)

### 构建项目

```bash
# 克隆仓库
git clone <repository-url>
cd zerokv-middleware

# 构建 (Debug模式)
./scripts/build.sh --debug --clean

# 构建 (Release模式)
./scripts/build.sh --release
```

### 运行测试

```bash
# 运行所有测试
cd build
ctest --output-on-failure

# 运行特定测试
./bin/test_server
./bin/test_client
```

## 代码规范

### C++ 代码规范

遵循 [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)

**关键要点:**
- 使用4空格缩进
- 类名使用 PascalCase (例如: `ZeroKVServer`)
- 函数名使用 PascalCase (例如: `GetStats`)
- 变量名使用 camelCase，成员变量后缀 `_` (例如: `kvTable_`)
- 常量使用 UPPER_CASE
- 头文件使用 `#ifndef` guard
- 所有public接口必须有文档注释

**示例:**

```cpp
/**
 * @brief Brief description
 *
 * Detailed description.
 *
 * @param param1 Description of param1
 * @return Description of return value
 */
Status MyFunction(const std::string& param1) {
    // Implementation
    return Status::OK();
}
```

### Python 代码规范

遵循 [PEP 8](https://www.python.org/dev/peps/pep-0008/)

**关键要点:**
- 使用4空格缩进
- 函数名和变量名使用 snake_case
- 类名使用 PascalCase
- 常量使用 UPPER_CASE
- 每行最多79字符
- 使用类型注解

**格式化工具:**

```bash
# 使用black格式化代码
black python/

# 使用flake8检查
flake8 python/
```

## 提交流程

### 分支策略

- `master`: 主分支，稳定版本
- `develop`: 开发分支，最新功能
- `feature/xxx`: 功能分支
- `bugfix/xxx`: 修复分支
- `hotfix/xxx`: 紧急修复分支

### 提交信息格式

使用约定式提交 (Conventional Commits):

```
<type>(<scope>): <subject>

<body>

<footer>
```

**类型 (type):**
- `feat`: 新功能
- `fix`: 修复bug
- `docs`: 文档更新
- `style`: 代码格式调整
- `refactor`: 重构
- `perf`: 性能优化
- `test`: 测试相关
- `chore`: 构建/工具相关

**示例:**

```
feat(server): add batch get support

Implement BatchGet API to retrieve multiple keys in a single request.
This improves performance when fetching multiple small objects.

Closes #123
```

### Pull Request 流程

1. **Fork 项目并创建分支**

```bash
git checkout -b feature/my-feature
```

2. **编写代码和测试**

- 确保所有测试通过
- 添加新功能的单元测试
- 更新相关文档

3. **提交代码**

```bash
git add .
git commit -m "feat(client): add async batch get"
```

4. **推送到远程**

```bash
git push origin feature/my-feature
```

5. **创建 Pull Request**

- 在GitHub上创建PR
- 填写PR模板
- 等待代码审查

### 代码审查标准

**必须满足:**
- 所有单元测试通过
- 代码覆盖率 >= 80%
- 无编译警告
- 遵循代码规范
- 有适当的文档
- 至少一个审查者批准

## 测试指南

### 单元测试

使用 Google Test 框架:

```cpp
#include <gtest/gtest.h>
#include "zerokv/zerokv_server.h"

TEST(ZeroKVServerTest, StartAndShutdown) {
    zerokv::ZeroKVServer server(0);
    auto status = server.Start("127.0.0.1", 50051);
    EXPECT_TRUE(status.ok());

    status = server.Shutdown();
    EXPECT_TRUE(status.ok());
}
```

### 集成测试

测试跨模块交互:

```cpp
TEST(IntegrationTest, ServerClientCommunication) {
    // Start server
    zerokv::ZeroKVServer server(0);
    server.Start("127.0.0.1", 50051);

    // Register data
    void* devPtr = malloc(1024);
    server.Put("test_key", devPtr, 1024);

    // Client connect and get
    zerokv::ZeroKVClient client(1);
    client.Connect("127.0.0.1", 50051);

    void* localPtr = malloc(1024);
    auto status = client.Get("test_key", localPtr, 1024);
    EXPECT_TRUE(status.ok());

    // Cleanup
    free(devPtr);
    free(localPtr);
}
```

### 性能测试

使用 Google Benchmark:

```cpp
static void BM_GetOperation(benchmark::State& state) {
    // Setup
    ZeroKVServer server(0);
    ZeroKVClient client(1);
    // ...

    for (auto _ : state) {
        client.Get("bench_key", ptr, size);
    }

    state.SetBytesProcessed(state.iterations() * size);
}
BENCHMARK(BM_GetOperation)->Range(1<<10, 1<<30);
```

## 文档贡献

### 更新文档

当添加新功能时，请更新:

1. **API文档** (`docs/api_reference.md`)
   - 添加新API的详细说明
   - 包含代码示例

2. **技术设计文档** (`docs/technical_design_document.md`)
   - 更新架构图
   - 说明设计决策

3. **README** (`README.md`)
   - 更新快速开始指南
   - 添加新功能说明

### 文档格式

- 使用Markdown格式
- 包含代码示例
- 添加图表说明复杂概念
- 中英文对照

## 性能优化指南

### 性能目标

- Get延迟 P95 < 50μs
- Get带宽 > 180GB/s (HCCS)
- QPS > 5M ops/s (小对象)

### 性能分析工具

```bash
# 使用perf分析
perf record -g ./bin/benchmark
perf report

# 内存泄漏检测
valgrind --leak-check=full ./bin/test_server

# 性能分析
./bin/benchmark --benchmark_format=json > results.json
```

### 优化检查清单

- [ ] 避免不必要的内存拷贝
- [ ] 使用零拷贝路径
- [ ] 减少锁竞争
- [ ] 内存对齐到512字节
- [ ] 复用连接和缓冲区
- [ ] 批量操作
- [ ] 异步API

## Bug 报告

### Bug 报告模板

```markdown
**环境信息:**
- OS: Ubuntu 20.04
- NPU: Ascend 910
- UCX version: 1.12.0

**描述:**
简要描述bug。

**复现步骤:**
1. 启动server
2. 运行client
3. 观察错误

**期望行为:**
描述期望的正确行为。

**实际行为:**
描述实际发生的错误。

**日志:**
```
粘贴相关日志
```

**其他信息:**
任何其他有助于诊断的信息。
```

## 发布流程

### 版本号规范

遵循 [Semantic Versioning](https://semver.org/):

- MAJOR.MINOR.PATCH (例如: 1.2.3)
- MAJOR: 不兼容的API变更
- MINOR: 向后兼容的新功能
- PATCH: 向后兼容的bug修复

### 发布检查清单

- [ ] 所有测试通过
- [ ] 性能测试达标
- [ ] 文档已更新
- [ ] CHANGELOG已更新
- [ ] 版本号已更新
- [ ] 创建git tag
- [ ] 构建发布包
- [ ] 发布说明已准备

## 社区

### 沟通渠道

- GitHub Issues: Bug报告和功能请求
- GitHub Discussions: 技术讨论
- Email: zerokv-dev@example.com

### 行为准则

- 尊重所有贡献者
- 欢迎建设性的反馈
- 不容忍骚扰和歧视
- 专注于技术讨论

## 许可证

贡献的代码将采用 Apache License 2.0 许可证。

提交PR即表示您同意将代码贡献给本项目，并遵守项目的许可证条款。

---

感谢您的贡献！
