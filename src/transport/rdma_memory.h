// RDMA Memory Operations
#pragma once

#include <ucp/api/ucp.h>
#include <cstdint>
#include <cstddef>

namespace zerokv {

struct RDMAMemory {
    void* local_addr;
    size_t length;
    uint32_t rkey;           // Remote key for RDMA access
    ucp_mem_h mem_h;         // UCX memory handle
    
    RDMAMemory() : local_addr(nullptr), length(0), rkey(0), mem_h(nullptr) {}
};

class RDMAMemoryManager {
public:
    RDMAMemoryManager(ucp_context_h context);
    ~RDMAMemoryManager();
    
    // Register memory for RDMA
    RDMAMemory register_memory(void* addr, size_t length);
    
    // Unregister memory
    void unregister_memory(RDMAMemory& mem);
    
    // Pack rkey for sending to remote
    std::vector<uint8_t> pack_rkey(const RDMAMemory& mem);
    
private:
    ucp_context_h context_;
};

} // namespace zerokv
