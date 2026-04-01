#pragma once

#include "kv/protocol.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace zerokv::kv::detail {

struct NodeInfo {
    enum class State {
        kAlive,
        kDead,
    };

    std::string node_id;
    std::string control_addr;
    std::string data_addr;
    std::string push_control_addr;
    std::string subscription_control_addr;
    uint64_t push_inbox_remote_addr = 0;
    std::vector<uint8_t> push_inbox_rkey;
    uint64_t push_inbox_capacity = 0;
    uint64_t last_heartbeat_ms = 0;
    State state = State::kAlive;
};

class MetadataStore {
public:
    bool register_node(NodeInfo node);
    bool update_heartbeat(const std::string& node_id, uint64_t now_ms);
    bool mark_node_dead(const std::string& node_id);

    [[nodiscard]] std::optional<NodeInfo> get_node(const std::string& node_id) const;

    bool put(KeyMetadata meta);
    [[nodiscard]] std::optional<KeyMetadata> get(const std::string& key) const;
    [[nodiscard]] std::optional<KeyMetadata> get_active(const std::string& key) const;
    bool erase(const std::string& key, const std::string& owner_node_id);
    bool subscribe(const std::string& subscriber_node_id, const std::string& key);
    bool unsubscribe(const std::string& subscriber_node_id, const std::string& key);
    [[nodiscard]] std::vector<std::string> subscribers_for(const std::string& key) const;

    [[nodiscard]] std::vector<std::string> list_keys() const;
    [[nodiscard]] std::vector<KeyMetadata> list_by_owner(const std::string& node_id) const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, NodeInfo> nodes_;
    std::unordered_map<std::string, KeyMetadata> keys_;
    std::unordered_map<std::string, std::vector<std::string>> subscriptions_;
};

}  // namespace zerokv::kv::detail
