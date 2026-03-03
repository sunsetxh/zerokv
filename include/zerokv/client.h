#ifndef ZEROKV_CLIENT_H
#define ZEROKV_CLIENT_H

#include "common.h"
#include "transport.h"
#include <string>
#include <vector>
#include <memory>

namespace zerokv {

// ZeroKV Client
class Client {
public:
    Client();
    ~Client();

    // Connection
    Status connect(const std::vector<std::string>& servers);
    void disconnect();

    // Basic operations
    Status put(const std::string& key, const void* value, size_t size);
    Status put(const std::string& key, const std::string& value);
    Status get(const std::string& key, void* buffer, size_t* size);
    Status get(const std::string& key, std::string* value);
    Status remove(const std::string& key);

    // User memory operations (zero-copy)
    Status put_user_mem(const std::string& key, void* remote_addr,
                        uint32_t rkey, size_t size);
    Status get_user_mem(const std::string& key, void* remote_addr,
                        uint32_t rkey, size_t size);

    // Set memory type
    void set_memory_type(MemoryType type);

private:
    std::unique_ptr<Transport> transport_;
    MemoryType memory_type_;
    bool connected_;
    std::vector<std::string> servers_;
    uint64_t request_id_;
};

} // namespace zerokv

#endif // ZEROKV_CLIENT_H
