#include "zerokv/connection_pool.h"
#include <algorithm>
#include <iostream>
#include <thread>

namespace zerokv {

// PooledConnection implementation
PooledConnection::PooledConnection(Transport* transport, const std::string& peer_addr)
    : transport_(transport), peer_addr_(peer_addr), valid_(true),
      created_at_(std::chrono::steady_clock::now()),
      last_used_(std::chrono::steady_clock::now()) {}

PooledConnection::~PooledConnection() {
    // Connection will be destroyed by pool
}

// ConnectionPool implementation
ConnectionPool::ConnectionPool(const std::vector<std::string>& servers, MemoryType memory_type)
    : servers_(servers), memory_type_(memory_type) {}

ConnectionPool::~ConnectionPool() {
    close();
}

Status ConnectionPool::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Create initial connections
    for (size_t i = 0; i < config_.min_connections; ++i) {
        auto conn = create_connection();
        if (conn && conn->is_valid()) {
            idle_queue_.push(std::move(conn));
            idle_connections_++;
        }
    }

    // Start maintenance thread
    running_ = true;
    maintenance_thread_ = std::thread([this]() { maintenance_loop(); });

    std::cout << "Connection pool initialized: "
              << "min=" << config_.min_connections
              << ", max=" << config_.max_connections << std::endl;

    return Status::OK;
}

std::unique_ptr<PooledConnection> ConnectionPool::acquire(const std::string& key) {
    // Simple hash-based server selection
    size_t idx = std::hash<std::string>{}(key) % servers_.size();
    return acquire(servers_[idx]);
}

std::unique_ptr<PooledConnection> ConnectionPool::acquire() {
    if (servers_.empty()) return nullptr;
    return acquire(servers_[0]);
}

std::unique_ptr<PooledConnection> ConnectionPool::create_connection() {
    if (closed_.load()) return nullptr;

    // Use round-robin or random server selection
    static std::atomic<size_t> server_idx{0};
    size_t idx = server_idx++ % servers_.size();

    auto transport = create_transport(memory_type_);
    if (!transport) return nullptr;

    Status status = transport->initialize();
    if (status != Status::OK) return nullptr;

    status = transport->connect(servers_[idx]);
    if (status != Status::OK) return nullptr;

    return std::make_unique<PooledConnection>(transport.release(), servers_[idx]);
}

void ConnectionPool::release(std::unique_ptr<PooledConnection> conn) {
    if (!conn || !conn->is_valid()) {
        destroy_connection(std::move(conn));
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (closed_.load()) {
        destroy_connection(std::move(conn));
        return;
    }

    conn->update_used();
    idle_queue_.push(std::move(conn));
    idle_connections_++;
    cv_.notify_one();
}

void ConnectionPool::destroy_connection(std::unique_ptr<PooledConnection> conn) {
    if (conn) {
        active_count_--;
    }
}

void ConnectionPool::maintenance_loop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(10));

        std::lock_guard<std::mutex> lock(mutex_);

        auto now = std::chrono::steady_clock::now();

        // Close excess idle connections
        while (idle_connections_ > config_.min_connections && !idle_queue_.empty()) {
            auto& conn = idle_queue_.front();
            auto idle_time = now - conn->last_used();

            if (idle_time > config_.max_idle_time || idle_connections_ > config_.max_connections) {
                idle_queue_.pop();
                idle_connections_--;
                // Connection will be destroyed when unique_ptr goes out of scope
            } else {
                break;
            }
        }

        // Create new connections if needed
        while (active_count_ + idle_connections_ < config_.min_connections) {
            auto conn = create_connection();
            if (conn && conn->is_valid()) {
                idle_queue_.push(std::move(conn));
                idle_connections_++;
            } else {
                break;
            }
        }
    }
}

void ConnectionPool::close() {
    running_ = false;
    closed_ = true;
    cv_.notify_all();

    if (maintenance_thread_.joinable()) {
        maintenance_thread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Close all idle connections
    while (!idle_queue_.empty()) {
        idle_queue_.pop();
        idle_connections_--;
    }
}

} // namespace zerokv
