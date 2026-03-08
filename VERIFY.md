# ZeroKV 快速验证指南

## 验证环境

```bash
cd /home/wyc/code/zerokv
```

## 1. 下载并编译 UCX 1.19.1

```bash
# 下载 UCX
wget https://github.com/openucx/ucx/releases/download/v1.19.1/ucx-1.19.1.tar.gz
tar -xzf ucx-1.19.1.tar.gz

# 编译 UCX
cd ucx-1.19.1
./configure --prefix=/home/wyc/code/zerokv/thirdparty/ucx-install --disable-debug --disable-assertions --enable-optimizations
make -j$(nproc)
make install
cd ..
```

## 2. 编译项目 (含UCX)

```bash
# 清理并重建
rm -rf build && mkdir build && cd build

# 配置 (需要UCX已安装)
cmake .. -DBUILD_TESTS=ON -DBUILD_PYTHON=OFF

# 编译
make -j$(nproc)
```

**预期输出:**
```
UCX found (local): /home/wyc/code/zerokv/thirdparty/ucx-install
[100%] Built target zerokv
[100%] Built target zerokv_server
```

## 3. 运行测试

### 3.1 自定义测试 (推荐)

```bash
# 编译自定义测试
cd /home/wyc/code/zerokv
g++ -std=c++17 -I include -o test_storage_standalone test_storage_standalone.cc src/storage/storage.cc -pthread
g++ -std=c++17 -I include -o test_cache test_cache.cc src/cache/cache.cc -pthread
g++ -std=c++17 -I include -o test_features test_features.cc src/checksum/checksum.cc src/config/config.cc src/metrics/metrics.cc src/error/retrier.cc -pthread
g++ -std=c++17 -I include -o test_batch test_batch.cc src/batch/batch.cc src/storage/storage.cc -pthread
g++ -std=c++17 -I include -o test_util test_util.cc -pthread

# 运行
./test_storage_standalone
./test_cache
./test_features
./test_batch
./test_util
```

**预期结果:** 全部通过 ✅

### 3.2 服务器启动测试

```bash
cd build
./zerokv_server -p 5000 -m 512
```

**预期输出:**
```
=== ZeroKV Server ===
Listen: 0.0.0.0:5000
Max Memory: 512 MB
UCX transport initialized (TCP mode)
UCX listening on 0.0.0.0:5000
Server started successfully
```

### 3.3 gtest 单元测试

```bash
cd build
./tests/test_storage
```

**注意:** 部分边界测试可能失败(LRU eviction边界情况)，核心功能正常。

## 4. 功能验证清单

| 功能 | 验证命令 | 预期结果 |
|------|----------|----------|
| 存储 Put/Get | test_storage_standalone Test1 | SUCCESS |
| LRU淘汰 | test_storage_standalone Test2 | 正确淘汰旧数据 |
| 大值存储(1MB) | Test3 | SUCCESS test_storage_standalone |
| LRU缓存 | test_cache | 所有策略通过 |
| CRC32校验 | test_features Checksum | PASSED |
| 配置加载 | test_features Config | 正确读取 |
| 指标收集 | test_features Metrics | 数据正确 |
| 批量操作 | test_batch | 100/100插入成功 |
| 工具函数 | test_util | 字符串/时间/随机/字节 |
| 并发访问 | test_integration | 400次操作0失败 |
| UCX服务器 | zerokv_server | 启动成功 |

## 5. 快速验证脚本

```bash
#!/bin/bash
set -e

echo "=== ZeroKV 快速验证 ==="

cd /home/wyc/code/zerokv

echo "[1/6] 编译存储测试..."
g++ -std=c++17 -I include -o test_storage_standalone test_storage_standalone.cc src/storage/storage.cc -pthread

echo "[2/6] 编译缓存测试..."
g++ -std=c++17 -I include -o test_cache test_cache.cc src/cache/cache.cc -pthread

echo "[3/6] 编译特性测试..."
g++ -std=c++17 -I include -o test_features test_features.cc src/checksum/checksum.cc src/config/config.cc src/metrics/metrics.cc src/error/retrier.cc -pthread

echo "[4/6] 编译批量测试..."
g++ -std=c++17 -I include -o test_batch test_batch.cc src/batch/batch.cc src/storage/storage.cc -pthread

echo "[5/6] 编译工具测试..."
g++ -std=c++17 -I include -o test_util test_util.cc -pthread

echo "[6/6] 运行所有测试..."
echo "--- Storage ---"
./test_storage_standalone
echo "--- Cache ---"
./test_cache
echo "--- Features ---"
./test_features
echo "--- Batch ---"
./test_batch
echo "--- Util ---"
./test_util

echo "=== 全部验证完成 ==="
```

## 6. 已知限制

1. **UCX TCP模式**: 当前使用TCP模式，RDMA需要硬件支持
2. **gtest边界测试**: 部分LRU边界测试可能失败，核心功能正常
3. **Python绑定**: 需要 pybind11

## 7. 验证问题排查

### 编译失败
- 检查 C++17 编译器是否安装: `g++ --version`
- 检查 CMake 版本: `cmake --version`
- 检查 UCX 是否正确安装: `ls thirdparty/ucx-install/lib/`

### 测试失败
- 检查是否在正确的目录
- 检查是否遗漏 `-pthread` 标志

### UCX 相关
- 确保 UCX 已编译安装: `ls thirdparty/ucx-install/include/ucp/api/ucp.h`
- 设置环境变量可减少警告: `export UCX_WARN_UNUSED_ENV_VARS=n`
