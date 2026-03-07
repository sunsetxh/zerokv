# Quick Start Guide: P2P Transport Library

## Installation

### Prerequisites

- Linux x86_64
- GCC >= 11 or Clang >= 14
- CMake >= 3.20
- UCX >= 1.14

### Build

```bash
# Clone and build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Install
sudo make install
```

### Dependencies

```bash
# Ubuntu/Debian
sudo apt install libucx-dev

# RHEL/CentOS
sudo dnf install ucx-devel

# From source
git clone https://github.com/openucx/ucx.git
cd ucx && ./contrib/configure-release && make -j$(nproc)
```

---

## C++ Quick Start

### 1. Basic Setup

```cpp
#include <p2p/p2p.h>

int main() {
    // Create configuration
    auto config = p2p::Config::builder()
        .set_transport("ucx")
        .set_num_workers(4)
        .build();

    // Create context
    auto ctx = p2p::Context::create(config);

    // Create worker
    auto worker = p2p::Worker::create(ctx);

    std::cout << "P2P context ready!\n";
    return 0;
}
```

### 2. Server (Listener)

```cpp
// Server: Listen for connections
auto listener = worker->listen("tcp://0.0.0.0:1234",
    [](p2p::Endpoint::Ptr ep) {
        // Handle incoming connection
        std::vector<uint8_t> buffer(4096);

        // Blocking receive
        auto future = ep->tag_recv(buffer.data(), buffer.size(), 42);
        auto [bytes, tag] = future.get();

        std::cout << "Received " << bytes << " bytes\n";
    });

std::cout << "Server listening on port 1234...\n";
std::cin.get();  // Wait for Ctrl+C
```

### 3. Client (Connector)

```cpp
// Client: Connect to server
auto endpoint = worker->connect("tcp://localhost:1234");

// Send message
std::vector<uint8_t> message = {1, 2, 3, 4, 5};
auto future = endpoint->tag_send(message.data(), message.size(), 42);

future.get();  // Wait for completion
std::cout << "Message sent!\n";
```

### 4. Async with Futures

```cpp
// Non-blocking send with callback
endpoint->tag_send(data, size, tag).on_complete([](p2p::Status st) {
    if (st.ok()) {
        std::cout << "Send complete!\n";
    } else {
        std::cout << "Send failed: " << st.message() << "\n";
    }
});

// Async receive with chaining
endpoint->tag_recv(buf, size, tag).then([](auto result) {
    auto [bytes, tag] = result;
    std::cout << "Got " << bytes << " bytes\n";
    return bytes;  // Pass to next chain
});
```

### 5. RDMA Put/Get

```cpp
// Register memory on both sides
auto local_reg = p2p::MemoryRegion::allocate(ctx, 1024 * 1024);

// Get remote key (sent via separate channel)
p2p::RemoteKey remote_key = /* from peer */;

// RDMA Put: Write to remote memory
auto future = endpoint->put(local_reg, 0,
                            remote_addr,  // Remote address
                            remote_key,   // Remote key
                            1024 * 1024); // Length
future.get();
```

### 6. Event Loop Integration

```cpp
// Python asyncio integration (via Python bindings)
import asyncio
import p2p

async def main():
    ctx = p2p.Context()
    worker = ctx.create_worker()

    # Register callback for UCX events
    worker.attach_to_event_loop(asyncio.get_event_loop())

    # Your async code here...
    await asyncio.sleep(1)

asyncio.run(main())
```

---

## Configuration

### Environment Variables

```bash
# Library options
export P2P_TRANSPORT=ucx       # Transport: ucx, tcp, rdma
export P2P_NUM_WORKERS=4       # Worker count (0=auto)
export P2P_LOG_LEVEL=info      # Debug, info, warn, error

# UCX options (passed through)
export UCX_TLS=rc,ud,sm,tcp   # Transport layers
export UCX_NET_DEVICES=eth0   # Network device
export UCX_RNDV_THRESH=8192   # Rendezvous threshold
```

### Programmatic

```cpp
auto config = p2p::Config::builder()
    .set_transport("tcp")
    .set_num_workers(8)
    .set_connect_timeout(std::chrono::seconds(30))
    .set("UCX_TLS", "tcp,sm")  // Pass UCX options
    .from_env()                 // Override from env
    .build();
```

---

## Common Patterns

### Connection Address Format

```
tcp://host:port        - TCP/IP
rdma://host:port       - RDMA (InfiniBand/RoCE)
self:rank              - Loopback (same process)
```

### Tag Matching

```cpp
// Exact match
ep->tag_recv(buf, size, 42, p2p::kTagMaskAll);

// Match any tag starting with context 0x1
uint64_t tag = p2p::make_tag(0x1, user_tag);
ep->tag_recv(buf, size, tag, 0xFFFFFFFF00000000);

// Wildcard receive
ep->tag_recv(buf, size, p2p::kTagAny, 0);
```

### Error Handling

```cpp
auto status = endpoint->tag_send(data, size, tag);

if (!status.ok()) {
    std::cerr << "Error: " << status.message() << "\n";
    // Handle error
}

// Or throw on error
status.throw_if_error();  // throws std::system_error
```

---

## Performance Tips

1. **Use registered memory** for messages > 256KB
2. **Pre-create endpoints** for high-frequency communication
3. **Use one worker per thread** for multi-threaded apps
4. **Enable registration cache** for repeated buffer reuse
5. **Match message size to protocol**:
   - < 8KB: Eager (fastest)
   - 8KB - 256KB: Eager zcopy
   - > 256KB: Rendezvous (zero-copy)

---

## Troubleshooting

### No RDMA devices found

```bash
# Check UCX transport
ucx_info -c

# Use TCP fallback
export UCX_TLS=tcp,sm,self
```

### Connection refused

```bash
# Check firewall
sudo iptables -L

# Verify port available
netstat -tlnp | grep 1234
```

### Performance issues

```bash
# Profile UCX
export UCX_LOG_LEVEL=debug

# Check NIC
ucx_info -d

# Benchmark
ucx_perftest -t tag_bw -n 1000 <device>
```

---

## Next Steps

- See [API Reference](api-reference.md) for full documentation
- See [Architecture Guide](../docs/architecture.md) for design details
- See [Examples](../examples/) for complete examples
