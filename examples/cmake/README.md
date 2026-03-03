# ZeroKV CMake Example

This example demonstrates how to use ZeroKV as a library in your CMake project.

## Quick Start

1. Build ZeroKV first:
   ```bash
   cd /path/to/zerokv
   mkdir build && cd build
   cmake .. -DBUILD_PYTHON=OFF
   make
   ```

2. Build this example:
   ```bash
   cd /path/to/zerokv/examples/cmake
   mkdir build && cd build
   cmake .. -Dzerokv_DIR=/path/to/zerokv/build
   make
   ```

3. Run:
   ```bash
   ./my_app
   ```

## Integration Guide

### Method 1: Using find_package

Add to your `CMakeLists.txt`:

```cmake
find_package(zerokv REQUIRED)
target_link_libraries(your_app PRIVATE zerokv::zerokv)
```

### Method 2: Using add_subdirectory

If you have ZeroKV as a subdirectory in your project:

```cmake
add_subdirectory(path/to/zerokv)
target_link_libraries(your_app PRIVATE zerokv)
```

## Requirements

- CMake 3.15+
- C++17 compatible compiler
- UCX 1.19+ (for network features)
