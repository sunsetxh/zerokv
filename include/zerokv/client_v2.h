#ifndef ZEROKV_CLIENT_V2_H
#define ZEROKV_CLIENT_V2_H

#include "zerokv/client.h"
#include "zerokv/connection_pool.h"
#include <memory>
#include <vector>
#include <string>

namespace zerokv {

// Advanced client with connection pool
class ClientV2 {
public:
    ClientV2();
    ~ClientV2();

    // Configuration
    struct Config {
        std::vector<std::string> servers;
        MemoryType memory_type = MemoryType::CPU;
        PoolConfig pool_config;
        bool enable_logging = true;
    };

    // Initialize with config
    Status initialize(const Config& config);

    // Connection
    void shutdown();

    // Basic operations
    Status put(const std::string& key, const void* value, size_t size);
    Status put(const std::string& key, const std::string& value);
    Status get(const std::string& key, void* buffer, size_t* size);
    Status get(const std::string& key, std::string* value);
    Status remove(const std::string& key);

    // Batch operations
    Status batch_put(const std::vector<std::pair<std::string, std::string>>& items);
    std::vector<std::string> batch_get(const std::vector<std::string>& keys);

    // User memory operations
    Status put_user_mem(const std::string& key, void* remote_addr,
                        uint32_t rkey, size_t size);
    Status get_user_mem(const std::string& key, void* remote_addr,
                        uint32_t rkey, size_t size);

    // Stats
    struct Stats {
        size_t active_connections;
        size_t idle_connections;
        uint64_t total_requests;
        uint64_t failed_requests;
    };
    Stats get_stats() const;

private:
    Config config_;
    std::unique_ptr<ConnectionPool> pool_;
    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> failed_requests_{0};
    bool initialized_ = false;
};

} // namespace zerokv

#endif // ZEROKV_CLIENT_V2_H
