#ifndef ZEROKV_TRANSPORT_H
#define ZEROKV_TRANSPORT_H

#include "common.h"
#include <memory>
#include <string>
#include <vector>

// UCX types - include full definitions
#include <ucp/api/ucp.h>
#include <ucs/type/status.h>
#include <uct/api/uct.h>

namespace zerokv {

// Forward declarations
class Transport {
public:
    virtual ~Transport() = default;

    virtual Status initialize() = 0;
    virtual Status connect(const std::string& peer_addr) = 0;
    virtual Status put(const std::string& key, const void* value, size_t size) = 0;
    virtual Status get(const std::string& key, void* buffer, size_t* size) = 0;
    virtual Status put_user_mem(const std::string& key, void* remote_addr,
                                uint32_t rkey, size_t size) = 0;
    virtual Status get_user_mem(const std::string& key, void* remote_addr,
                                uint32_t rkey, size_t size) = 0;
    virtual Status delete_key(const std::string& key) = 0;
    virtual void shutdown() = 0;
};

// Factory function
std::unique_ptr<Transport> create_transport(MemoryType type);

} // namespace zerokv

#endif // ZEROKV_TRANSPORT_H
