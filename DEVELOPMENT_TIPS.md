# ZeroKV 开发提示 (Development Tips)

> **重要**: 每次开始工作前请阅读本文档！

## 🔴 核心提醒

### 1. ⚠️ UCX 只能在 Linux 下编译

**关键事实**：
- ❌ macOS **无法**编译真实 UCX（缺少 Linux 内核特性）
- ✅ macOS 只能使用 **stub 模式**开发
- ✅ 所有真实 UCX 验证**必须**在 Docker 容器中进行

**正确的验证流程**：
```bash
# ❌ 错误：在 macOS 上验证真实 UCX
cd build && cmake .. -DUSE_UCX_STUB=OFF && make  # 会失败！

# ✅ 正确：在 Docker 容器中验证
docker exec zerokv-ucx-test bash -c "
  cd /workspace/build &&
  cmake .. -DUSE_UCX_STUB=OFF &&
  make -j5 &&
  ctest -V
"
```

### 2. 🔄 Docker 容器代码是挂载的（非常重要！）

**关键事实**：
- 容器的 `/workspace` 和主机的 `zerokv` 目录是**同一个目录**
- 通过 `-v` 挂载实现，**不是复制**

**这意味着**：
```
主机修改 ➡️  容器内立即同步
容器内修改 ➡️  主机立即同步
```

**推荐工作流程**：
```bash
# 1. 在 macOS 用 IDE 编辑代码（体验好）
vim src/client/ucx_control_client.cpp

# 2. 在容器内编译（支持真实 UCX）
docker exec zerokv-ucx-test bash -c "cd /workspace/build && make -j5"

# 3. 在 macOS 提交代码（Git 体验好）
git add -A && git commit -m "your message"
```

**不要**在容器内编辑代码（vim 体验差），也**不要**在容器内 git 操作。

### 3. 📦 Docker 容器信息

**容器名称**: `zerokv-ucx-test`
**操作系统**: Ubuntu 24.04
**挂载路径**: `/Users/wangyuchao/code/openyuanrong/zerokv` → `/workspace`

**检查容器状态**：
```bash
docker ps -a | grep zerokv-ucx-test
```

**如果容器未运行**：
```bash
docker start zerokv-ucx-test
```

**如果容器不存在**：
```bash
docker run -d --name zerokv-ucx-test \
  -v /Users/wangyuchao/code/openyuanrong/zerokv:/workspace \
  -w /workspace \
  ubuntu:24.04 tail -f /dev/null

# 安装依赖
docker exec zerokv-ucx-test bash -c "
  apt-get update && apt-get install -y \
    cmake g++ autoconf libtool make
"
```

## 🎯 开发工作流程

### 标准流程（99% 的情况）

```bash
# Step 1: macOS 编辑代码
# 使用 VS Code / CLion / Vim 等编辑器

# Step 2: 容器内编译和测试
docker exec zerokv-ucx-test bash -c "
  cd /workspace/build &&
  make -j5 &&
  ctest -V
"

# Step 3: macOS 提交代码
git add -A
git commit -m "feat: your feature"
git push
```

### 完整重新构建

```bash
docker exec zerokv-ucx-test bash -c "
  cd /workspace &&
  rm -rf build &&
  mkdir build &&
  cd build &&
  cmake .. -DBUILD_PYTHON=OFF &&
  make -j5 &&
  ctest -V
"
```

### 快速测试单个文件

```bash
# 编译单个目标
docker exec zerokv-ucx-test bash -c "cd /workspace/build && make zerokv_client -j5"

# 运行单个测试
docker exec zerokv-ucx-test bash -c "cd /workspace/build && ./bin/test_ucx_control_client"
```

## 📝 编译配置速查

### 三种构建模式

| 模式 | CMake 命令 | 适用场景 | 网络功能 |
|------|-----------|---------|---------|
| Stub | `-DUSE_UCX_STUB=ON` | macOS 开发 | ❌ 模拟 |
| 自动构建（默认） | `-DUSE_UCX_STUB=OFF` | Linux 生产/测试 | ✅ 真实 |
| 自定义 UCX | `-DUCX_ROOT=/path` | 高级优化 | ✅ 真实 |

### macOS 开发配置

```bash
# macOS 只能用 stub 模式
mkdir build && cd build
cmake .. -DUSE_UCX_STUB=ON -DBUILD_PYTHON=OFF
make -j8
```

### Linux/Docker 生产配置

```bash
# 默认会自动构建 UCX 1.20.0
mkdir build && cd build
cmake .. -DBUILD_PYTHON=OFF
make -j5
```

## 🧪 测试相关

### 运行所有测试

```bash
docker exec zerokv-ucx-test bash -c "cd /workspace/build && ctest -V"
```

### 运行单个测试

```bash
docker exec zerokv-ucx-test bash -c "cd /workspace/build && ./bin/test_p2p_mock"
docker exec zerokv-ucx-test bash -c "cd /workspace/build && ./bin/test_ucx_control_server"
docker exec zerokv-ucx-test bash -c "cd /workspace/build && ./bin/test_ucx_control_client"
```

### 调试失败的测试

```bash
# 详细输出
docker exec zerokv-ucx-test bash -c "cd /workspace/build && ctest --output-on-failure"

# 只运行失败的测试
docker exec zerokv-ucx-test bash -c "cd /workspace/build && ctest --rerun-failed"

# 运行特定的测试用例
docker exec zerokv-ucx-test bash -c "
  cd /workspace/build &&
  ./bin/test_ucx_control_client --gtest_filter=UCXControlClientTest.InitializationTest
"
```

## 📂 目录结构

```
zerokv/
├── include/zerokv/          # 公共头文件
│   ├── logger.h            # 日志系统
│   ├── ucx_control_client.h
│   └── ucx_control_server.h
├── src/
│   ├── common/             # 通用代码
│   │   ├── logger.cpp
│   │   ├── p2p_ucx_mock.cpp
│   │   ├── ucx_stub.cpp   # UCX stub 实现
│   │   └── ucx_stub.h
│   ├── server/             # 服务器代码
│   │   └── ucx_control_server.cpp
│   └── client/             # 客户端代码
│       └── ucx_control_client.cpp
├── tests/unit/             # 单元测试
│   ├── test_p2p_mock.cpp
│   ├── test_ucx_control_server.cpp
│   └── test_ucx_control_client.cpp
├── build/                  # 构建目录（容器内创建）
│   └── ucx-install/       # 自动构建的 UCX 1.20.0
├── CMakeLists.txt
├── README.md
├── TASKS.md               # 任务清单
├── WORK_LOG.md           # 工作日志
└── DEVELOPMENT_TIPS.md   # 本文档
```

## 🐛 常见问题

### Q1: macOS 编译报错 "UCX not found"

**原因**: 忘记设置 `-DUSE_UCX_STUB=ON`

**解决**:
```bash
cd build
cmake .. -DUSE_UCX_STUB=ON -DBUILD_PYTHON=OFF
make -j8
```

### Q2: 容器内测试端口冲突

**现象**: "Address already in use"

**原因**: 真实 UCX 环境下端口释放需要时间

**解决**:
```bash
# 等待几秒后重试
sleep 5
docker exec zerokv-ucx-test bash -c "cd /workspace/build && ctest -V"

# 或者重启容器
docker restart zerokv-ucx-test
```

### Q3: 容器不存在或已停止

**检查**:
```bash
docker ps -a | grep zerokv-ucx-test
```

**启动**:
```bash
docker start zerokv-ucx-test
```

**重建**:
```bash
docker rm -f zerokv-ucx-test  # 删除旧容器
# 重新创建（见上面的命令）
```

### Q4: 首次编译很慢

**原因**: 需要构建 UCX 1.20.0（约 3-5 分钟）和 Protobuf

**正常现象**: 第一次编译慢，后续增量编译快

**加速方法**: 使用 `-j5` 或更高并行度

### Q5: 修改代码后容器内看不到

**检查挂载**:
```bash
docker exec zerokv-ucx-test bash -c "ls -la /workspace/src/client/"
```

**如果看不到**: 容器挂载可能失败，需要重建容器

## 🔧 调试技巧

### 1. 查看编译详细输出

```bash
docker exec zerokv-ucx-test bash -c "cd /workspace/build && make VERBOSE=1"
```

### 2. 只编译不测试

```bash
docker exec zerokv-ucx-test bash -c "cd /workspace/build && make -j5"
```

### 3. 清理重新构建

```bash
docker exec zerokv-ucx-test bash -c "cd /workspace && rm -rf build"
```

### 4. 查看 UCX 安装路径

```bash
docker exec zerokv-ucx-test bash -c "ls -la /workspace/build/ucx-install/"
```

### 5. 检查日志输出级别

在测试中设置日志级别：
```cpp
zerokv::LogManager::Instance().SetLevel(zerokv::LogLevel::DEBUG);
```

## 📌 提交规范

### Conventional Commits 格式

```bash
feat: add new feature
fix: fix bug in xxx
docs: update documentation
refactor: refactor code structure
test: add unit tests
chore: update build config
```

### 提交前检查清单

- [ ] 代码在容器内编译通过
- [ ] 单元测试通过（至少 95%）
- [ ] 日志使用 LOG_* 宏（不用 cout/cerr）
- [ ] 更新 TASKS.md 进度
- [ ] 更新 WORK_LOG.md（重要变更）
- [ ] Commit message 符合规范

## 🎯 性能注意事项

### 编译并行度

```bash
# macOS (M1/M2): 8-10 核
make -j8

# Docker 容器: 建议 5 核（避免内存不足）
make -j5
```

### 测试超时

如果测试卡住：
```bash
# 设置超时
docker exec zerokv-ucx-test bash -c "cd /workspace/build && timeout 60 ctest -V"
```

## 📚 相关文档

- **README.md**: 项目概述和快速开始
- **TASKS.md**: 任务列表和进度跟踪
- **WORK_LOG.md**: 详细的工作日志
- **docs/**: 技术设计文档

---

**最后更新**: 2025-02-01
**维护者**: Claude AI Assistant

**记住**：
1. ⚠️ UCX 只能在 Linux 下编译
2. 🔄 容器代码是挂载的，修改会同步
3. 📦 每次工作前检查容器状态
4. 🧪 所有真实 UCX 验证必须在容器内进行
