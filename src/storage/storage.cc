#include "zerokv/storage.h"
#include <algorithm>
#include <cstring>

namespace zerokv {

StorageEngine::StorageEngine(size_t max_memory)
    : max_memory_(max_memory), used_memory_(0) {

    // Pre-allocate memory pool
    size_t chunk_count = std::min(max_memory / CHUNK_SIZE, MAX_CHUNK_COUNT);
    memory_pool_.reserve(chunk_count);

    for (size_t i = 0; i < chunk_count; ++i) {
        auto chunk = std::make_unique<Chunk>();
        chunk->data = new uint8_t[CHUNK_SIZE];
        chunk->size = CHUNK_SIZE;
        chunk->in_use = false;
        memory_pool_.push_back(std::move(chunk));
    }
}

StorageEngine::~StorageEngine() {
    // Cleanup is automatic with unique_ptr
}

// Allocate chunks from pool
std::vector<Chunk*> StorageEngine::allocate_chunks(size_t size) {
    std::vector<Chunk*> chunks;
    if (size == 0) return chunks;  // No data to store

    size_t needed = (size + CHUNK_SIZE - 1) / CHUNK_SIZE;

    for (auto& chunk : memory_pool_) {
        if (!chunk->in_use && chunks.size() < needed) {
            chunk->in_use = true;
            chunks.push_back(chunk.get());
        }
        if (chunks.size() >= needed) break;
    }

    return chunks;
}

// Release chunks
void StorageEngine::release_chunks(ValueHandle& handle) {
    for (auto* chunk : handle.chunks) {
        chunk->in_use = false;
    }
    handle.chunks.clear();
}

// Count free chunks
size_t StorageEngine::free_chunk_count() const {
    size_t count = 0;
    for (auto& chunk : memory_pool_) {
        if (!chunk->in_use) count++;
    }
    return count;
}

// Evict LRU entries - fixed version
void StorageEngine::evict_lru(size_t needed_size) {
    if (needed_size == 0) return;  // Nothing to allocate

    size_t needed_chunks = (needed_size + CHUNK_SIZE - 1) / CHUNK_SIZE;

    while (!lru_list_.empty()) {
        // Check if we have enough memory AND chunks
        bool have_enough = (used_memory_ + needed_size <= max_memory_);
        have_enough = have_enough && (free_chunk_count() >= needed_chunks);

        if (have_enough) break;

        // Evict one LRU item
        auto it = lru_list_.begin();
        std::string evict_key = *it;

        auto cache_it = cache_.find(evict_key);
        if (cache_it != cache_.end()) {
            release_chunks(cache_it->second.value);
            used_memory_ -= cache_it->second.value.total_size;
            cache_.erase(cache_it);
        }
        lru_list_.erase(it);
    }
}

// Put operation - fixed version
Status StorageEngine::put(const std::string& key, const void* value, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Free existing key if present
    auto existing = cache_.find(key);
    if (existing != cache_.end()) {
        release_chunks(existing->second.value);
        used_memory_ -= existing->second.value.total_size;
        lru_list_.erase(existing->second.lru_iter);
        cache_.erase(existing);
    }

    // Evict LRU until we have enough space
    evict_lru(size);

    // Allocate chunks
    auto chunks = allocate_chunks(size);
    if (chunks.empty()) {
        return Status::OUT_OF_MEMORY;
    }

    // Copy data to chunks
    const uint8_t* src = static_cast<const uint8_t*>(value);
    size_t offset = 0;
    for (auto* chunk : chunks) {
        size_t copy_size = std::min(size - offset, CHUNK_SIZE);
        std::memcpy(chunk->data, src + offset, copy_size);
        offset += copy_size;
    }

    // Store in cache
    LRUCacheEntry entry;
    entry.key = key;
    entry.value.chunks = std::move(chunks);
    entry.value.total_size = size;
    entry.value.is_user_memory = false;
    entry.lru_iter = lru_list_.insert(lru_list_.end(), key);

    cache_[key] = std::move(entry);
    used_memory_ += size;

    return Status::OK;
}

// Get operation
Status StorageEngine::get(const std::string& key, void* buffer, size_t* size) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(key);
    if (it == cache_.end()) {
        return Status::NOT_FOUND;
    }

    // Update LRU - move to end
    lru_list_.erase(it->second.lru_iter);
    it->second.lru_iter = lru_list_.insert(lru_list_.end(), key);

    // Check buffer size
    if (*size < it->second.value.total_size) {
        *size = it->second.value.total_size;
        return Status::ERROR;
    }

    // Copy data from chunks
    uint8_t* dst = static_cast<uint8_t*>(buffer);
    size_t offset = 0;
    for (auto* chunk : it->second.value.chunks) {
        size_t copy_size = std::min(it->second.value.total_size - offset, CHUNK_SIZE);
        std::memcpy(dst + offset, chunk->data, copy_size);
        offset += copy_size;
    }

    *size = it->second.value.total_size;
    return Status::OK;
}

// Delete operation
Status StorageEngine::delete_key(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(key);
    if (it == cache_.end()) {
        return Status::NOT_FOUND;
    }

    release_chunks(it->second.value);
    used_memory_ -= it->second.value.total_size;
    lru_list_.erase(it->second.lru_iter);
    cache_.erase(it);

    return Status::OK;
}

// User memory operations (placeholder - requires RDMA)
Status StorageEngine::put_user_mem(const std::string& key, void* user_addr,
                                    uint32_t rkey, size_t size) {
    // TODO: Implement RDMA put to user memory
    return Status::ERROR;
}

Status StorageEngine::get_user_mem(const std::string& key, void* user_addr,
                                    uint32_t rkey, size_t size) {
    // TODO: Implement RDMA get to user memory
    return Status::ERROR;
}

} // namespace zerokv
