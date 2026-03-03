#include "zerokv/transport.h"
#include "zerokv/storage.h"
#include <memory>

namespace zerokv {

// Factory function to create transport instances
std::unique_ptr<Transport> create_transport(MemoryType type) {
    // For now, return nullptr as we don't have a full transport implementation
    // In a real implementation, this would create UCX-based transport
    return nullptr;
}

} // namespace zerokv
