# Quick Start: ZeroKV Development

## Prerequisites

- C++17 compatible compiler (GCC 9+ or Clang 10+)
- CMake 3.15+
- Python 3.10+ (for Python bindings)
- Git
- RDMA hardware (optional, for RDMA features)

## Initial Setup

### 1. Clone and Prepare

```bash
git clone https://github.com/zerokv/zerokv.git
cd zerokv
```

### 2. Build UCX (Required for network features)

```bash
# Option A: Use provided script
./scripts/download_ucx.sh

# Option B: Manual build
cd thirdparty/ucx
./autogen.sh
./configure --prefix=/usr/local --enable-optimizations
make -j$(nproc)
sudo make install
sudo ldconfig
```

### 3. Build Project

```bash
mkdir build && cd build
cmake .. -DBUILD_TESTS=ON -DBUILD_PYTHON=ON
make -j$(nproc)
```

### 4. Run Tests

```bash
# Custom simple tests
./test_storage_standalone
./test_cache
./test_features
./test_batch

# gtest (requires installation)
sudo apt-get install libgtest-dev
cd build
cmake .. -DBUILD_TESTS=ON
make test
```

## Common Tasks

### Add New Module

1. Create header in `include/zerokv/`
2. Create implementation in `src/`
3. Add to `CMakeLists.txt`
4. Add tests

### Run Benchmarks

```bash
./build/benchmark/zerokv_benchmark
```

## Troubleshooting

**UCX not found**: Ensure UCX is installed and `ldconfig` has been run
**Python bindings fail**: Install pybind11: `pip install pybind11`
**Tests fail**: Check that all required libraries are installed

## Next Steps

See `docs/` for architecture and API documentation.
