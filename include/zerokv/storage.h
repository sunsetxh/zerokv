#ifndef ZEROKV_STORAGE_H
#define ZEROKV_STORAGE_H

#include "common.h"
#include <string>
#include <memory>
#include <unordered_map>
#include <list>
#include <mutex>
#include <cstring>

namespace zerokv {

// Chunk size for memory pool (64KB)
constexpr size_t CHUNK_SIZE = 64 * 1024;
constexpr size_t MAX_CHUNK_COUNT = 16384; // Max 1GB total

// Memory chunk
struct Chunk {
    uint8_t* data;
    size_t size;
    bool in_use;

    Chunk() : data(nullptr), size(0), in_use(false) {}
};

// Value handle
struct ValueHandle {
    std::vector<Chunk*> chunks;
    size_t total_size;
    bool is_user_memory;

    ValueHandle() : total_size(0), is_user_memory(false) {}
};

// LRU cache entry
struct LRUCacheEntry {
    std::string key;
    ValueHandle value;
    std::list<std::string>::iterator lru_iter;
};

// Storage engine
class StorageEngine {
public:
    StorageEngine(size_t max_memory = 1024 * 1024 * 1024); // Default 1GB
    ~StorageEngine();

    // Operations
    Status put(const std::string& key, const void* value, size_t size);
    Status get(const std::string& key, void* buffer, size_t* size);
    Status delete_key(const std::string& key);

    // User memory operations
    Status put_user_mem(const std::string& key, void* user_addr,
                        uint32_t rkey, size_t size);
    Status get_user_mem(const std::string& key, void* user_addr,
                        uint32_t rkey, size_t size);

    // Stats
    size_t used_memory() const { return used_memory_; }
    size_t item_count() const { return cache_.size(); }

private:
    // Memory pool
    std::vector<std::unique_ptr<Chunk>> memory_pool_;
    std::list<std::string> lru_list_;
    std::unordered_map<std::string, LRUCacheEntry> cache_;
    size_t max_memory_;
    size_t used_memory_;
    std::mutex mutex_;

    // Allocate chunks from pool
    std::vector<Chunk*> allocate_chunks(size_t size);

    // Count free chunks
    size_t free_chunk_count() const;

    // Release chunks
    void release_chunks(ValueHandle& handle);

    // Evict LRU entries
    void evict_lru(size_t needed_size);
};

} // namespace zerokv

#endif // ZEROKV_STORAGE_H
