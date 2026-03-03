#include "zerokv/cache.h"
#include <chrono>
#include <algorithm>
#include <random>
#include <cassert>

namespace zerokv {

// LRU Cache implementation
LRUCache::LRUCache(size_t max_size) : max_size_(max_size) {}

bool LRUCache::get(const std::string& key, std::string* value) {
    auto it = data_.find(key);
    if (it == data_.end()) {
        stats_.misses++;
        return false;
    }

    // Update LRU
    lru_list_.remove(key);
    lru_list_.push_back(key);
    it->second.access_time = std::chrono::steady_clock::now().time_since_epoch().count();

    *value = it->second.value;
    stats_.hits++;
    return true;
}

bool LRUCache::set(const std::string& key, const std::string& value) {
    // Evict if needed
    if (data_.size() >= max_size_ && data_.find(key) == data_.end()) {
        evict();
    }

    auto it = data_.find(key);
    if (it != data_.end()) {
        it->second.value = value;
        lru_list_.remove(key);
    } else {
        data_[key] = {value, 0};
        stats_.insertions++;
    }

    lru_list_.push_back(key);
    return true;
}

bool LRUCache::del(const std::string& key) {
    auto it = data_.find(key);
    if (it == data_.end()) {
        return false;
    }

    lru_list_.remove(key);
    data_.erase(it);
    return true;
}

bool LRUCache::exists(const std::string& key) {
    return data_.find(key) != data_.end();
}

void LRUCache::clear() {
    data_.clear();
    lru_list_.clear();
}

CacheStats LRUCache::stats() const {
    return stats_;
}

void LRUCache::set_max_size(size_t max_items) {
    max_size_ = max_items;
    while (data_.size() > max_size_) {
        evict();
    }
}

void LRUCache::evict() {
    if (lru_list_.empty()) return;

    std::string key = lru_list_.front();
    lru_list_.pop_front();
    data_.erase(key);
    stats_.evictions++;
}

// TTL Cache implementation
TTLCache::TTLCache(size_t max_size, uint64_t ttl_ms)
    : max_size_(max_size), ttl_ms_(ttl_ms) {}

bool TTLCache::get(const std::string& key, std::string* value) {
    auto it = data_.find(key);
    if (it == data_.end()) {
        stats_.misses++;
        return false;
    }

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    if (now > it->second.expires_at) {
        data_.erase(it);
        stats_.misses++;
        return false;
    }

    *value = it->second.value;
    stats_.hits++;
    return true;
}

bool TTLCache::set(const std::string& key, const std::string& value) {
    if (data_.size() >= max_size_) {
        // Simple eviction: remove expired first, then oldest
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        for (auto it = data_.begin(); it != data_.end();) {
            if (now > it->second.expires_at) {
                it = data_.erase(it);
                stats_.evictions++;
            } else {
                ++it;
            }
        }
    }

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    data_[key] = {value, now + ttl_ms_};
    stats_.insertions++;
    return true;
}

bool TTLCache::del(const std::string& key) {
    return data_.erase(key) > 0;
}

bool TTLCache::exists(const std::string& key) {
    auto it = data_.find(key);
    if (it == data_.end()) return false;

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    if (now > it->second.expires_at) {
        data_.erase(it);
        return false;
    }
    return true;
}

void TTLCache::clear() {
    data_.clear();
}

CacheStats TTLCache::stats() const {
    return stats_;
}

void TTLCache::set_max_size(size_t max_items) {
    max_size_ = max_items;
}

// LFU Cache implementation
LFUCache::LFUCache(size_t max_size) : max_size_(max_size) {}

bool LFUCache::get(const std::string& key, std::string* value) {
    auto it = data_.find(key);
    if (it == data_.end()) {
        stats_.misses++;
        return false;
    }

    // Increment frequency
    it->second.frequency++;

    *value = it->second.value;
    stats_.hits++;
    return true;
}

bool LFUCache::set(const std::string& key, const std::string& value) {
    // Evict if needed
    if (data_.size() >= max_size_ && data_.find(key) == data_.end()) {
        evict();
    }

    auto it = data_.find(key);
    if (it != data_.end()) {
        it->second.value = value;
    } else {
        data_[key] = {value, 1};
        stats_.insertions++;
    }
    return true;
}

bool LFUCache::del(const std::string& key) {
    return data_.erase(key) > 0;
}

bool LFUCache::exists(const std::string& key) {
    return data_.find(key) != data_.end();
}

void LFUCache::clear() {
    data_.clear();
}

CacheStats LFUCache::stats() const {
    return stats_;
}

void LFUCache::set_max_size(size_t max_items) {
    max_size_ = max_items;
    while (data_.size() > max_size_) {
        evict();
    }
}

void LFUCache::evict() {
    if (data_.empty()) return;

    // Find entry with minimum frequency
    auto min_it = data_.begin();
    for (auto it = data_.begin(); it != data_.end(); ++it) {
        if (it->second.frequency < min_it->second.frequency) {
            min_it = it;
        }
    }

    data_.erase(min_it);
    stats_.evictions++;
}

// FIFO Cache implementation
FIFOCache::FIFOCache(size_t max_size)
    : max_size_(max_size), insert_counter_(0) {}

bool FIFOCache::get(const std::string& key, std::string* value) {
    auto it = data_.find(key);
    if (it == data_.end()) {
        stats_.misses++;
        return false;
    }

    *value = it->second.value;
    stats_.hits++;
    return true;
}

bool FIFOCache::set(const std::string& key, const std::string& value) {
    // Evict if needed
    if (data_.size() >= max_size_ && data_.find(key) == data_.end()) {
        evict();
    }

    auto it = data_.find(key);
    if (it != data_.end()) {
        it->second.value = value;
    } else {
        data_[key] = {value, insert_counter_++};
        fifo_list_.push_back(key);
        stats_.insertions++;
    }
    return true;
}

bool FIFOCache::del(const std::string& key) {
    auto it = data_.find(key);
    if (it == data_.end()) {
        return false;
    }

    fifo_list_.remove(key);
    data_.erase(it);
    return true;
}

bool FIFOCache::exists(const std::string& key) {
    return data_.find(key) != data_.end();
}

void FIFOCache::clear() {
    data_.clear();
    fifo_list_.clear();
}

CacheStats FIFOCache::stats() const {
    return stats_;
}

void FIFOCache::set_max_size(size_t max_items) {
    max_size_ = max_items;
    while (data_.size() > max_size_) {
        evict();
    }
}

void FIFOCache::evict() {
    if (fifo_list_.empty()) return;

    std::string key = fifo_list_.front();
    fifo_list_.pop_front();
    data_.erase(key);
    stats_.evictions++;
}

// Random Cache implementation
RandomCache::RandomCache(size_t max_size) : max_size_(max_size) {}

bool RandomCache::get(const std::string& key, std::string* value) {
    auto it = data_.find(key);
    if (it == data_.end()) {
        stats_.misses++;
        return false;
    }

    *value = it->second.value;
    stats_.hits++;
    return true;
}

bool RandomCache::set(const std::string& key, const std::string& value) {
    // Evict if needed
    if (data_.size() >= max_size_ && data_.find(key) == data_.end()) {
        evict();
    }

    auto it = data_.find(key);
    if (it != data_.end()) {
        it->second.value = value;
    } else {
        data_[key] = {value};
        stats_.insertions++;
    }
    return true;
}

bool RandomCache::del(const std::string& key) {
    return data_.erase(key) > 0;
}

bool RandomCache::exists(const std::string& key) {
    return data_.find(key) != data_.end();
}

void RandomCache::clear() {
    data_.clear();
}

CacheStats RandomCache::stats() const {
    return stats_;
}

void RandomCache::set_max_size(size_t max_items) {
    max_size_ = max_items;
    while (data_.size() > max_size_) {
        evict();
    }
}

void RandomCache::evict() {
    if (data_.empty()) return;

    static std::random_device rd;
    static std::mt19937 gen(rd());

    std::uniform_int_distribution<> dist(0, static_cast<int>(data_.size()) - 1);
    auto it = data_.begin();
    std::advance(it, dist(gen));

    data_.erase(it);
    stats_.evictions++;
}

// Factory
std::unique_ptr<Cache> create_cache(EvictionPolicy policy, size_t max_size) {
    switch (policy) {
        case EvictionPolicy::LRU:
            return std::make_unique<LRUCache>(max_size);
        case EvictionPolicy::LFU:
            return std::make_unique<LFUCache>(max_size);
        case EvictionPolicy::FIFO:
            return std::make_unique<FIFOCache>(max_size);
        case EvictionPolicy::RANDOM:
            return std::make_unique<RandomCache>(max_size);
        default:
            return std::make_unique<LRUCache>(max_size);
    }
}

} // namespace zerokv
