// Cache policies interface
#pragma once

#include <string>
#include <vector>
#include <functional>
#include <list>
#include <memory>
#include <unordered_map>

namespace zerokv {

// Cache eviction policies
enum class EvictionPolicy {
    LRU,      // Least Recently Used
    LFU,      // Least Frequently Used
    FIFO,     // First In First Out
    RANDOM    // Random eviction
};

// Cache statistics
struct CacheStats {
    size_t hits = 0;
    size_t misses = 0;
    size_t evictions = 0;
    size_t insertions = 0;

    double hit_rate() const {
        size_t total = hits + misses;
        return total > 0 ? (double)hits / total : 0;
    }
};

// Cache interface
class Cache {
public:
    virtual ~Cache() = default;

    // Get value
    virtual bool get(const std::string& key, std::string* value) = 0;

    // Set value
    virtual bool set(const std::string& key, const std::string& value) = 0;

    // Delete
    virtual bool del(const std::string& key) = 0;

    // Check exists
    virtual bool exists(const std::string& key) = 0;

    // Clear all
    virtual void clear() = 0;

    // Get stats
    virtual CacheStats stats() const = 0;

    // Set max size
    virtual void set_max_size(size_t max_items) = 0;
};

// LRU Cache implementation
class LRUCache : public Cache {
public:
    explicit LRUCache(size_t max_size = 1000);

    bool get(const std::string& key, std::string* value) override;
    bool set(const std::string& key, const std::string& value) override;
    bool del(const std::string& key) override;
    bool exists(const std::string& key) override;
    void clear() override;
    CacheStats stats() const override;
    void set_max_size(size_t max_items) override;

private:
    void evict();

    struct Entry {
        std::string value;
        uint64_t access_time;
    };

    size_t max_size_;
    std::unordered_map<std::string, Entry> data_;
    std::list<std::string> lru_list_;
    mutable CacheStats stats_;
};

// TTL Cache (time-to-live)
class TTLCache : public Cache {
public:
    TTLCache(size_t max_size, uint64_t ttl_ms);

    bool get(const std::string& key, std::string* value) override;
    bool set(const std::string& key, const std::string& value) override;
    bool del(const std::string& key) override;
    bool exists(const std::string& key) override;
    void clear() override;
    CacheStats stats() const override;
    void set_max_size(size_t max_items) override;

private:
    struct Entry {
        std::string value;
        uint64_t expires_at;
    };

    size_t max_size_;
    uint64_t ttl_ms_;
    std::unordered_map<std::string, Entry> data_;
    mutable CacheStats stats_;
};

// LFU Cache (Least Frequently Used)
class LFUCache : public Cache {
public:
    explicit LFUCache(size_t max_size = 1000);

    bool get(const std::string& key, std::string* value) override;
    bool set(const std::string& key, const std::string& value) override;
    bool del(const std::string& key) override;
    bool exists(const std::string& key) override;
    void clear() override;
    CacheStats stats() const override;
    void set_max_size(size_t max_items) override;

private:
    void evict();

    struct Entry {
        std::string value;
        uint64_t frequency;
    };

    size_t max_size_;
    std::unordered_map<std::string, Entry> data_;
    mutable CacheStats stats_;
};

// FIFO Cache (First In First Out)
class FIFOCache : public Cache {
public:
    explicit FIFOCache(size_t max_size = 1000);

    bool get(const std::string& key, std::string* value) override;
    bool set(const std::string& key, const std::string& value) override;
    bool del(const std::string& key) override;
    bool exists(const std::string& key) override;
    void clear() override;
    CacheStats stats() const override;
    void set_max_size(size_t max_items) override;

private:
    void evict();

    struct Entry {
        std::string value;
        uint64_t insert_order;
    };

    size_t max_size_;
    uint64_t insert_counter_;
    std::unordered_map<std::string, Entry> data_;
    std::list<std::string> fifo_list_;
    mutable CacheStats stats_;
};

// Random Cache (Random eviction)
class RandomCache : public Cache {
public:
    explicit RandomCache(size_t max_size = 1000);

    bool get(const std::string& key, std::string* value) override;
    bool set(const std::string& key, const std::string& value) override;
    bool del(const std::string& key) override;
    bool exists(const std::string& key) override;
    void clear() override;
    CacheStats stats() const override;
    void set_max_size(size_t max_items) override;

private:
    void evict();

    struct Entry {
        std::string value;
    };

    size_t max_size_;
    std::unordered_map<std::string, Entry> data_;
    mutable CacheStats stats_;
};

// Factory
std::unique_ptr<Cache> create_cache(EvictionPolicy policy, size_t max_size);

} // namespace zerokv
