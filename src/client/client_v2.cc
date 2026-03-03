#include "zerokv/client_v2.h"
#include "zerokv/logging.h"

namespace zerokv {

ClientV2::ClientV2() {}

ClientV2::~ClientV2() {
    shutdown();
}

Status ClientV2::initialize(const Config& config) {
    if (initialized_) {
        return Status::OK;
    }

    config_ = config;

    // Create connection pool
    pool_ = std::make_unique<ConnectionPool>(config.servers, config.memory_type);
    pool_->set_config(config.pool_config);

    Status status = pool_->initialize();
    if (status != Status::OK) {
        ZEROKV_ERROR("Failed to initialize connection pool");
        return status;
    }

    initialized_ = true;
    ZEROKV_INFO("ClientV2 initialized with ", config.servers.size(), " servers");
    return Status::OK;
}

void ClientV2::shutdown() {
    if (pool_) {
        pool_->close();
        pool_.reset();
    }
    initialized_ = false;
    ZEROKV_INFO("ClientV2 shutdown");
}

Status ClientV2::put(const std::string& key, const void* value, size_t size) {
    if (!initialized_) return Status::ERROR;

    auto conn = pool_->acquire(key);
    if (!conn || !conn->is_valid()) {
        failed_requests_++;
        return Status::ERROR;
    }

    Status status = conn->get()->put(key, value, size);
    total_requests_++;

    if (status != Status::OK) {
        failed_requests_++;
        conn->invalidate();
    }

    pool_->release(std::move(conn));
    return status;
}

Status ClientV2::put(const std::string& key, const std::string& value) {
    return put(key, value.data(), value.size());
}

Status ClientV2::get(const std::string& key, void* buffer, size_t* size) {
    if (!initialized_) return Status::ERROR;

    auto conn = pool_->acquire(key);
    if (!conn || !conn->is_valid()) {
        failed_requests_++;
        return Status::ERROR;
    }

    Status status = conn->get()->get(key, buffer, size);
    total_requests_++;

    if (status != Status::OK) {
        failed_requests_++;
    }

    pool_->release(std::move(conn));
    return status;
}

Status ClientV2::get(const std::string& key, std::string* value) {
    if (!initialized_) return Status::ERROR;

    // First get size
    size_t size = 0;
    Status status = get(key, nullptr, &size);
    if (status != Status::OK) return status;

    value->resize(size);
    return get(key, value->data(), &size);
}

Status ClientV2::remove(const std::string& key) {
    if (!initialized_) return Status::ERROR;

    auto conn = pool_->acquire(key);
    if (!conn || !conn->is_valid()) {
        failed_requests_++;
        return Status::ERROR;
    }

    Status status = conn->get()->delete_key(key);
    total_requests_++;

    if (status != Status::OK) {
        failed_requests_++;
    }

    pool_->release(std::move(conn));
    return status;
}

Status ClientV2::batch_put(const std::vector<std::pair<std::string, std::string>>& items) {
    Status result = Status::OK;
    for (const auto& [key, value] : items) {
        Status s = put(key, value);
        if (s != Status::OK) result = s;
    }
    return result;
}

std::vector<std::string> ClientV2::batch_get(const std::vector<std::string>& keys) {
    std::vector<std::string> results;
    results.reserve(keys.size());

    for (const auto& key : keys) {
        std::string value;
        Status s = get(key, &value);
        if (s == Status::OK) {
            results.push_back(std::move(value));
        } else {
            results.push_back("");
        }
    }
    return results;
}

Status ClientV2::put_user_mem(const std::string& key, void* remote_addr,
                               uint32_t rkey, size_t size) {
    if (!initialized_) return Status::ERROR;

    auto conn = pool_->acquire(key);
    if (!conn || !conn->is_valid()) {
        return Status::ERROR;
    }

    Status status = conn->get()->put_user_mem(key, remote_addr, rkey, size);
    total_requests_++;

    if (status != Status::OK) {
        failed_requests_++;
    }

    pool_->release(std::move(conn));
    return status;
}

Status ClientV2::get_user_mem(const std::string& key, void* remote_addr,
                               uint32_t rkey, size_t size) {
    if (!initialized_) return Status::ERROR;

    auto conn = pool_->acquire(key);
    if (!conn || !conn->is_valid()) {
        return Status::ERROR;
    }

    Status status = conn->get()->get_user_mem(key, remote_addr, rkey, size);
    total_requests_++;

    if (status != Status::OK) {
        failed_requests_++;
    }

    pool_->release(std::move(conn));
    return status;
}

ClientV2::Stats ClientV2::get_stats() const {
    Stats stats;
    if (pool_) {
        stats.active_connections = pool_->active_connections();
        stats.idle_connections = pool_->idle_connections();
    } else {
        stats.active_connections = 0;
        stats.idle_connections = 0;
    }
    stats.total_requests = total_requests_.load();
    stats.failed_requests = failed_requests_.load();
    return stats;
}

} // namespace zerokv
