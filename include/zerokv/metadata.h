#ifndef ZEROKV_METADATA_H
#define ZEROKV_METADATA_H

#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace zerokv {

// Metadata store interface
class MetadataStore {
public:
    virtual ~MetadataStore() = default;

    // Key routing
    virtual bool set_key_route(const std::string& key, const std::string& node_id) = 0;
    virtual bool get_key_route(const std::string& key, std::string* node_id) = 0;
    virtual bool delete_key_route(const std::string& key) = 0;

    // Node management
    virtual bool register_node(const std::string& node_id, const std::string& addr) = 0;
    virtual bool unregister_node(const std::string& node_id) = 0;
    virtual bool get_all_nodes(std::vector<std::pair<std::string, std::string>>* nodes) = 0;

    // Watch for changes
    using WatchCallback = std::function<void(const std::string& key, const std::string& value)>;
    virtual void watch(const std::string& key, WatchCallback callback) = 0;
};

// Redis-based metadata store
class RedisMetadataStore : public MetadataStore {
public:
    RedisMetadataStore(const std::string& addr);
    ~RedisMetadataStore() override;

    bool set_key_route(const std::string& key, const std::string& node_id) override;
    bool get_key_route(const std::string& key, std::string* node_id) override;
    bool delete_key_route(const std::string& key) override;

    bool register_node(const std::string& node_id, const std::string& addr) override;
    bool unregister_node(const std::string& node_id) override;
    bool get_all_nodes(std::vector<std::pair<std::string, std::string>>* nodes) override;

    void watch(const std::string& key, WatchCallback callback) override;

private:
    std::string addr_;
    // Redis connection would be stored here
};

// Etcd-based metadata store
class EtcdMetadataStore : public MetadataStore {
public:
    EtcdMetadataStore(const std::string& endpoints);
    ~EtcdMetadataStore() override;

    bool set_key_route(const std::string& key, const std::string& node_id) override;
    bool get_key_route(const std::string& key, std::string* node_id) override;
    bool delete_key_route(const std::string& key) override;

    bool register_node(const std::string& node_id, const std::string& addr) override;
    bool unregister_node(const std::string& node_id) override;
    bool get_all_nodes(std::vector<std::pair<std::string, std::string>>* nodes) override;

    void watch(const std::string& key, WatchCallback callback) override;

private:
    std::string endpoints_;
    // Etcd client would be stored here
};

// Factory
std::unique_ptr<MetadataStore> create_metadata_store(
    const std::string& type,
    const std::string& addr);

} // namespace zerokv

#endif // ZEROKV_METADATA_H
