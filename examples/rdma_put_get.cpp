/// @file rdma_put_get.cpp
/// @brief RDMA put/get example.
///
/// This example demonstrates:
///   - Memory registration
///   - Remote key serialization
///   - RDMA put/get operations
///
/// Note: RDMA requires actual hardware (InfiniBand, RoCE) or proper setup.
///       This example shows the API usage but cannot run without RDMA hardware.

#include <p2p/config.h>
#include <p2p/worker.h>
#include <p2p/endpoint.h>
#include <p2p/memory.h>

#include <iostream>
#include <vector>
#include <cstring>

using namespace p2p;

int main() {
    // Create config
    auto config = Config::builder()
        .set_transport("tcp")  // Note: RDMA requires rdma transport
        .build();

    // Config is always valid with default values

    auto ctx = Context::create(config);
    if (!ctx) {
        std::cerr << "Failed to create context\n";
        return 1;
    }

    auto worker = Worker::create(ctx);
    if (!worker) {
        std::cerr << "Failed to create worker\n";
        return 1;
    }

    // Allocate registered memory
    constexpr size_t kBufferSize = 4096;

    auto local_region = MemoryRegion::allocate(ctx, kBufferSize);
    if (!local_region) {
        std::cerr << "Failed to allocate memory region\n";
        return 1;
    }

    std::cout << "Allocated " << local_region->length() << " bytes at "
              << local_region->address() << "\n";

    // Initialize data
    auto* buffer = static_cast<char*>(local_region->address());
    std::memset(buffer, 'A', kBufferSize);

    // Get remote key for this memory region
    auto rkey = local_region->remote_key();

    if (rkey.empty()) {
        std::cout << "Remote key is empty (expected without connected endpoint)\n";
        std::cout << "Note: RDMA operations require:\n";
        std::cout << "  1. RDMA-capable hardware (InfiniBand, RoCE)\n";
        std::cout << "  2. Connected endpoint to pack rkey\n";
        std::cout << "  3. Exchange rkey with remote peer\n";
    } else {
        std::cout << "Remote key size: " << rkey.size() << " bytes\n";
    }

    // Demonstrate what RDMA put would look like
    std::cout << "\nRDMA put/get API example:\n";
    std::cout << "  ep->put(local_region, remote_addr, rkey, length);\n";
    std::cout << "  ep->get(local_region, remote_addr, rkey, length);\n";
    std::cout << "  ep->flush();  // Ensure completion\n";

    // Note about RDMA hardware
    std::cout << "\nTo test RDMA:\n";
    std::cout << "  1. Use RDMA hardware (InfiniBand, RoCE)\n";
    std::cout << "  2. Set P2P_TRANSPORTS=rdma,rc,verbs\n";
    std::cout << "  3. Establish connection and exchange rkeys\n";

    return 0;
}
