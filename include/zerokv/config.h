// Configuration management
#pragma once

#include <string>
#include <map>
#include <memory>
#include <variant>
#include <vector>

namespace zerokv {

// Config value types
using ConfigValue = std::variant<
    std::string,
    int,
    int64_t,
    double,
    bool,
    std::vector<std::string>
>;

// Configuration class
class Config {
public:
    Config() = default;

    // Load from file
    bool load(const std::string& filename);

    // Save to file
    bool save(const std::string& filename) const;

    // Get value
    template<typename T>
    T get(const std::string& key, const T& default_value = T{}) const {
        auto it = values_.find(key);
        if (it == values_.end()) {
            return default_value;
        }
        try {
            return std::get<T>(it->second);
        } catch (...) {
            return default_value;
        }
    }

    // Set value
    template<typename T>
    void set(const std::string& key, const T& value) {
        values_[key] = value;
    }

    // Check if key exists
    bool has(const std::string& key) const;

    // Get all keys
    std::vector<std::string> keys() const;

    // Clear all
    void clear() { values_.clear(); }

    // Merge another config
    void merge(const Config& other);

    // Server config
    struct ServerConfig {
        std::string bind_addr = "0.0.0.0";
        uint16_t port = 5000;
        size_t max_memory = 1024ULL * 1024 * 1024; // 1GB
        int worker_threads = 4;
    };

    // Client config
    struct ClientConfig {
        std::vector<std::string> servers;
        int connection_pool_size = 16;
        int timeout_ms = 5000;
        int retry_count = 3;
    };

    // Parse to struct
    ServerConfig server() const;
    ClientConfig client() const;

private:
    std::map<std::string, ConfigValue> values_;
};

} // namespace zerokv
