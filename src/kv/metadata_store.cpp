#include "kv/metadata_store.h"

#include <algorithm>

namespace axon::kv::detail {

bool MetadataStore::register_node(NodeInfo node) {
    if (node.node_id.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mu_);
    auto& slot = nodes_[node.node_id];
    slot = std::move(node);
    slot.state = NodeInfo::State::kAlive;
    return true;
}

bool MetadataStore::update_heartbeat(const std::string& node_id, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        return false;
    }
    it->second.last_heartbeat_ms = now_ms;
    it->second.state = NodeInfo::State::kAlive;
    return true;
}

bool MetadataStore::mark_node_dead(const std::string& node_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        return false;
    }
    it->second.state = NodeInfo::State::kDead;

    for (auto key_it = keys_.begin(); key_it != keys_.end();) {
        if (key_it->second.owner_node_id == node_id) {
            key_it = keys_.erase(key_it);
        } else {
            ++key_it;
        }
    }
    return true;
}

std::optional<NodeInfo> MetadataStore::get_node(const std::string& node_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool MetadataStore::put(KeyMetadata meta) {
    if (meta.key.empty() || meta.owner_node_id.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mu_);
    auto node_it = nodes_.find(meta.owner_node_id);
    if (node_it == nodes_.end() || node_it->second.state != NodeInfo::State::kAlive) {
        return false;
    }
    auto existing = keys_.find(meta.key);
    if (existing != keys_.end() && existing->second.owner_node_id != meta.owner_node_id) {
        return false;
    }
    keys_[meta.key] = std::move(meta);
    return true;
}

std::optional<KeyMetadata> MetadataStore::get(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = keys_.find(key);
    if (it == keys_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<KeyMetadata> MetadataStore::get_active(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = keys_.find(key);
    if (it == keys_.end()) {
        return std::nullopt;
    }
    auto node_it = nodes_.find(it->second.owner_node_id);
    if (node_it == nodes_.end() || node_it->second.state != NodeInfo::State::kAlive) {
        return std::nullopt;
    }
    return it->second;
}

bool MetadataStore::erase(const std::string& key, const std::string& owner_node_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = keys_.find(key);
    if (it == keys_.end() || it->second.owner_node_id != owner_node_id) {
        return false;
    }
    keys_.erase(it);
    return true;
}

std::vector<std::string> MetadataStore::list_keys() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> result;
    result.reserve(keys_.size());
    for (const auto& [key, meta] : keys_) {
        auto node_it = nodes_.find(meta.owner_node_id);
        if (node_it != nodes_.end() && node_it->second.state == NodeInfo::State::kAlive) {
            result.push_back(key);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<KeyMetadata> MetadataStore::list_by_owner(const std::string& node_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<KeyMetadata> result;
    for (const auto& [key, meta] : keys_) {
        if (meta.owner_node_id == node_id) {
            result.push_back(meta);
        }
    }
    std::sort(result.begin(), result.end(), [](const KeyMetadata& lhs, const KeyMetadata& rhs) {
        return lhs.key < rhs.key;
    });
    return result;
}

}  // namespace axon::kv::detail
