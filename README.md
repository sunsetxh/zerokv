# ZeroKV

High-performance distributed KV storage based on UCX, with HCCL/NCCL extension support.

## Features

- **High Performance**: RDMA-based communication using UCX
- **Large Value Support**: 1KB to 1GB value storage
- **Multi-Memory Support**: CPU (UCX), Huawei NPU (HCCL), NVIDIA GPU (NCCL)
- **Zero-Copy**: RDMA direct memory access for GPU显存
- **Python & C++ APIs**: Easy integration
- **Built-in Features**: Connection pooling, Metrics, Checksum, Configuration

## Architecture

```
ZeroKV Architecture
├── API Layer: Python, C++, HCCL/NCCL
├── Client SDK: Connection Pool, Routing
├── Storage Engine: Memory Pool, LRU Cache
└── Transport: UCX, HCCL, NCCL
```

## Quick Start

### C++

```cpp
#include "zerokv/client.h"

zerokv::Client client;
client.connect({"localhost:5000"});
client.put("key", "value");
std::string val = client.get("key");
```

### Python

```python
from zerokv import ZeroKV

client = ZeroKV()
client.connect(["localhost:5000"])
client.put("key", "value")
value = client.get("key")
```

## Build

```bash
mkdir build && cd build
cmake .. -DBUILD_TESTS=ON
make -j$(nproc)
```

## Start Server

### Quick Start

```bash
# Build完成后启动服务端
./build/zerokv_server -a 0.0.0.0 -p 5000 -m 65536

# 或者使用默认配置 (1GB内存)
./build/zerokv_server
```

### Server Options

| Option | Default | Description |
|--------|---------|-------------|
| `-a <addr>` | 0.0.0.0 | Listen address |
| `-p <port>` | 5000 | Server port |
| `-m <MB>` | 1024 | Max memory in MB |

### Docker

```bash
docker run -p 5000:5000 zerokv:latest
```

## Modules

| Module | Description |
|--------|-------------|
| storage | Memory pool + LRU cache |
| transport | UCX/HCCL/NCCL |
| checksum | CRC32 data integrity |
| config | Configuration management |
| metrics | Performance monitoring |

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Development Guide](docs/DEVELOPMENT.md)
- [Meeting Notes](docs/meeting-*.md)

## License

MIT
