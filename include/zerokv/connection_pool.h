#ifndef ZEROKV_CONNECTION_POOL_H
#define ZEROKV_CONNECTION_POOL_H

#include "zerokv/transport.h"
#include <memory>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <thread>

namespace zerokv {

// Connection pool configuration
struct PoolConfig {
    size_t min_connections = 1;
    size_t max_connections = 16;
    std::chrono::milliseconds max_idle_time{30000};  // 30 seconds
    std::chrono::seconds max_lifetime{3600};          // 1 hour
    size_t max_queue_size = 1000;
};

// Connection wrapper
class PooledConnection {
public:
    PooledConnection(Transport* transport, const std::string& peer_addr);
    ~PooledConnection();

    Transport* get() { return transport_.get(); }
    bool is_valid() const { return valid_; }
    void invalidate() { valid_ = false; }

    // Connection health
    std::chrono::steady_clock::time_point created_at() const { return created_at_; }
    std::chrono::steady_clock::time_point last_used() const { return last_used_; }
    void update_used() { last_used_ = std::chrono::steady_clock::now(); }

private:
    std::unique_ptr<Transport> transport_;
    std::string peer_addr_;
    bool valid_;
    std::chrono::steady_clock::time_point created_at_;
    std::chrono::steady_clock::time_point last_used_;
};

// Connection pool
class ConnectionPool {
public:
    ConnectionPool(const std::vector<std::string>& servers, MemoryType memory_type);
    ~ConnectionPool();

    // Initialize pool
    Status initialize();

    // Get connection from pool
    std::unique_ptr<PooledConnection> acquire(const std::string& key);
    std::unique_ptr<PooledConnection> acquire();

    // Return connection to pool
    void release(std::unique_ptr<PooledConnection> conn);

    // Pool management
    void close();
    size_t active_connections() const { return active_count_; }
    size_t idle_connections() const { return idle_connections_; }

    // Configuration
    void set_config(const PoolConfig& config) { config_ = config; }
    const PoolConfig& get_config() const { return config_; }

private:
    std::vector<std::string> servers_;
    MemoryType memory_type_;
    PoolConfig config_;

    std::queue<std::unique_ptr<PooledConnection>> idle_queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<size_t> active_count_{0};
    std::atomic<size_t> idle_connections_{0};
    std::atomic<bool> closed_{false};

    // Background maintenance
    std::thread maintenance_thread_;
    std::atomic<bool> running_{false};

    void maintenance_loop();
    std::unique_ptr<PooledConnection> create_connection();
    void destroy_connection(std::unique_ptr<PooledConnection> conn);
};

} // namespace zerokv

#endif // ZEROKV_CONNECTION_POOL_H
