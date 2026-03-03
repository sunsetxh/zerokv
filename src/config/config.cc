#include "zerokv/config.h"
#include <fstream>
#include <sstream>
#include <iostream>

namespace zerokv {

bool Config::load(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Failed to open config file: " << filename << std::endl;
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        // Parse key=value
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);

        // Trim whitespace
        auto trim = [](std::string& s) {
            size_t start = s.find_first_not_of(" \t");
            size_t end = s.find_last_not_of(" \t");
            if (start == std::string::npos) {
                s = "";
            } else {
                s = s.substr(start, end - start + 1);
            }
        };

        trim(key);
        trim(value);

        // Parse value type
        if (value == "true" || value == "false") {
            set(key, value == "true");
        } else if (value.find('.') != std::string::npos) {
            set(key, std::stod(value));
        } else if (value.find_first_not_of("0123456789-") == std::string::npos) {
            if (value.length() > 9) {
                set(key, std::stoll(value));
            } else {
                set(key, std::stoi(value));
            }
        } else {
            set(key, value);
        }
    }

    return true;
}

bool Config::save(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
        return false;
    }

    for (const auto& [key, value] : values_) {
        file << key << "=";
        std::visit([&file](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::vector<std::string>>) {
                for (size_t i = 0; i < v.size(); i++) {
                    if (i > 0) file << ",";
                    file << v[i];
                }
            } else {
                file << v;
            }
        }, value);
        file << "\n";
    }

    return true;
}

bool Config::has(const std::string& key) const {
    return values_.find(key) != values_.end();
}

std::vector<std::string> Config::keys() const {
    std::vector<std::string> result;
    for (const auto& [key, _] : values_) {
        result.push_back(key);
    }
    return result;
}

void Config::merge(const Config& other) {
    for (const auto& [key, value] : other.values_) {
        values_[key] = value;
    }
}

Config::ServerConfig Config::server() const {
    ServerConfig cfg;
    cfg.bind_addr = get("server.bind_addr", cfg.bind_addr);
    cfg.port = static_cast<uint16_t>(get("server.port", static_cast<int>(cfg.port)));
    cfg.max_memory = static_cast<size_t>(get("server.max_memory", static_cast<int>(cfg.max_memory / (1024 * 1024)))) * 1024 * 1024;
    cfg.worker_threads = get("server.worker_threads", cfg.worker_threads);
    return cfg;
}

Config::ClientConfig Config::client() const {
    ClientConfig cfg;
    cfg.connection_pool_size = get("client.pool_size", cfg.connection_pool_size);
    cfg.timeout_ms = get("client.timeout_ms", cfg.timeout_ms);
    cfg.retry_count = get("client.retry_count", cfg.retry_count);

    // Parse servers
    std::string servers_str = get<std::string>("client.servers", "");
    if (!servers_str.empty()) {
        std::stringstream ss(servers_str);
        std::string server;
        while (std::getline(ss, server, ',')) {
            cfg.servers.push_back(server);
        }
    }

    return cfg;
}

} // namespace zerokv
