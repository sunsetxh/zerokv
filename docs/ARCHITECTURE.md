# ZeroKV Architecture

## 1. System Architecture

- API Layer: Python, C++, HCCL/NCCL
- Client SDK: Connection Pool, Routing, Load Balancing
- Storage Engine: Memory Pool, Index, LRU
- Transport Layer: UCX, HCCL, NCCL

## 2. Core Modules

### 2.1 Storage Engine
- Memory pool (64KB chunks)
- Skip list / B+Tree index
- LRU eviction
- 1KB~1GB value support

### 2.2 Transport Layer
- UCX: CPU memory
- HCCL: Huawei NPU
- NCCL: NVIDIA GPU

### 2.3 Connection Pool
- Min/max connections
- Health check
- Auto-reconnect

## 3. Performance

- Small value (1KB): >100K QPS
- Large value (1GB): Full RDMA bandwidth
- Latency: <1ms

