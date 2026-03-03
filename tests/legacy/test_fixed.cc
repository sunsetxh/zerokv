// Fixed storage engine test
#include <iostream>
#include <chrono>
#include <random>
#include <cstring>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <list>
#include <mutex>

class StorageEngine {
public:
    static constexpr size_t CHUNK_SIZE = 64 * 1024;  // 64KB chunks

    struct Chunk {
        uint8_t* data;
        bool in_use;
        Chunk() : data(new uint8_t[CHUNK_SIZE]), in_use(false) {}
        ~Chunk() { delete[] data; }
    };

    struct ValueData {
        std::vector<Chunk*> chunks;
        size_t size;
    };

    struct CacheEntry {
        ValueData data;
        std::list<std::string>::iterator lru_iter;
    };

    StorageEngine(size_t max_memory) : max_memory_(max_memory), used_memory_(0) {
        // Pre-allocate chunks
        size_t chunk_count = max_memory / CHUNK_SIZE;
        for (size_t i = 0; i < chunk_count; ++i) {
            chunks_.push_back(std::make_unique<Chunk>());
        }
        std::cout << "[Storage] Initialized: " << chunk_count << " chunks, "
                  << (max_memory / 1024 / 1024) << "MB" << std::endl;
    }

    ~StorageEngine() {
        std::cout << "[Storage] Shutdown" << std::endl;
    }

    bool put(const std::string& key, const void* value, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Free existing key if present
        auto existing = cache_.find(key);
        if (existing != cache_.end()) {
            free_value(existing->second.data);
            lru_list_.erase(existing->second.lru_iter);
            cache_.erase(existing);
        }

        // Evict LRU until we have enough space
        while (used_memory_ + size > max_memory_ && !lru_list_.empty()) {
            std::string lru_key = lru_list_.front();
            lru_list_.pop_front();

            auto it = cache_.find(lru_key);
            if (it != cache_.end()) {
                free_value(it->second.data);
                used_memory_ -= it->second.data.size;
                cache_.erase(it);
            }
        }

        // Allocate chunks
        ValueData data;
        data.size = size;
        size_t needed = (size + CHUNK_SIZE - 1) / CHUNK_SIZE;

        for (auto& chunk : chunks_) {
            if (!chunk->in_use && data.chunks.size() < needed) {
                chunk->in_use = true;
                data.chunks.push_back(chunk.get());
            }
            if (data.chunks.size() >= needed) break;
        }

        if (data.chunks.size() < needed) {
            std::cerr << "[ERROR] Out of memory: need " << needed << " chunks, have " << data.chunks.size() << std::endl;
            return false;
        }

        // Copy data
        const uint8_t* src = static_cast<const uint8_t*>(value);
        size_t offset = 0;
        for (auto* chunk : data.chunks) {
            size_t copy = std::min(size - offset, CHUNK_SIZE);
            memcpy(chunk->data, src + offset, copy);
            offset += copy;
        }

        // Store
        CacheEntry entry;
        entry.data = std::move(data);
        entry.lru_iter = lru_list_.insert(lru_list_.end(), key);
        cache_[key] = std::move(entry);
        used_memory_ += size;

        return true;
    }

    bool get(const std::string& key, void* buffer, size_t* size) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = cache_.find(key);
        if (it == cache_.end()) {
            return false;
        }

        if (*size < it->second.data.size) {
            *size = it->second.data.size;
            return false;
        }

        // Move to front of LRU
        lru_list_.erase(it->second.lru_iter);
        it->second.lru_iter = lru_list_.insert(lru_list_.end(), key);

        // Copy data
        uint8_t* dst = static_cast<uint8_t*>(buffer);
        size_t offset = 0;
        for (auto* chunk : it->second.data.chunks) {
            size_t copy = std::min(it->second.data.size - offset, CHUNK_SIZE);
            memcpy(dst + offset, chunk->data, copy);
            offset += copy;
        }

        *size = it->second.data.size;
        return true;
    }

    bool remove(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = cache_.find(key);
        if (it == cache_.end()) {
            return false;
        }

        free_value(it->second.data);
        used_memory_ -= it->second.data.size;
        lru_list_.erase(it->second.lru_iter);
        cache_.erase(it);

        return true;
    }

    size_t used_memory() const { return used_memory_; }
    size_t item_count() const { return cache_.size(); }

private:
    void free_value(ValueData& data) {
        for (auto* chunk : data.chunks) {
            chunk->in_use = false;
        }
        data.chunks.clear();
    }

    std::vector<std::unique_ptr<Chunk>> chunks_;
    std::unordered_map<std::string, CacheEntry> cache_;
    std::list<std::string> lru_list_;
    size_t max_memory_;
    size_t used_memory_;
    std::mutex mutex_;
};

void test_basic() {
    std::cout << "\n=== Test 1: Basic Put/Get ===" << std::endl;
    StorageEngine storage(1024 * 1024);

    std::string key = "test_key";
    std::string value = "Hello ZeroKV!";

    bool ok = storage.put(key, value.data(), value.size());
    std::cout << "[Put] " << (ok ? "SUCCESS" : "FAILED") << std::endl;

    char buffer[1024];
    size_t size = sizeof(buffer);
    ok = storage.get(key, buffer, &size);
    std::cout << "[Get] " << (ok ? "SUCCESS" : "FAILED") << std::endl;
    buffer[size] = '\0';
    std::cout << "[Value] " << buffer << std::endl;
}

void test_lru() {
    std::cout << "\n=== Test 2: LRU Eviction ===" << std::endl;
    StorageEngine storage(256 * 1024); // 256KB

    // Insert items until eviction starts
    int inserted = 0;
    for (int i = 0; i < 100; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string value(2000, 'x'); // 2KB each
        if (storage.put(key, value.data(), value.size())) {
            inserted++;
        } else {
            std::cout << "[Info] Stopped at " << inserted << " items" << std::endl;
            break;
        }
    }
    std::cout << "[Info] Inserted: " << inserted << " items" << std::endl;
    std::cout << "[Info] Storage used: " << (storage.used_memory() / 1024) << " KB" << std::endl;

    // First items should be evicted
    char buffer[1024];
    size_t size = sizeof(buffer);
    bool ok = storage.get("key_0", buffer, &size);
    std::cout << "[Test] Get key_0: " << (ok ? "FOUND" : "EVICTED") << std::endl;

    // Last item should exist
    ok = storage.get("key_99", buffer, &size);
    std::cout << "[Test] Get key_99: " << (ok ? "FOUND" : "NOT FOUND") << std::endl;
}

void test_large() {
    std::cout << "\n=== Test 3: Large Value ===" << std::endl;
    StorageEngine storage(10 * 1024 * 1024); // 10MB

    std::string key = "large";
    std::string value(1024 * 1024, 'L'); // 1MB

    auto start = std::chrono::high_resolution_clock::now();
    bool ok = storage.put(key, value.data(), value.size());
    auto end = std::chrono::high_resolution_clock::now();
    auto dur = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "[Put] " << (ok ? "SUCCESS" : "FAILED") << std::endl;
    std::cout << "[Perf] 1MB put: " << dur.count() << " us" << std::endl;
}

void test_benchmark() {
    std::cout << "\n=== Test 4: Benchmark ===" << std::endl;
    StorageEngine storage(50 * 1024 * 1024); // 50MB

    const int N = 5000;
    std::string value(1024, 'x');

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N; ++i) {
        std::string key = "key_" + std::to_string(i);
        storage.put(key, value.data(), value.size());
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "[Perf] Put " << N << " x 1KB: " << dur.count() << " ms" << std::endl;
    std::cout << "[Perf] Throughput: " << (N * 1000 / dur.count()) << " ops/sec" << std::endl;
    std::cout << "[Info] Storage used: " << (storage.used_memory() / 1024) << " KB" << std::endl;
}

int main() {
    std::cout << "╔════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║          ZeroKV Storage Engine Test (Fixed)             ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════╝" << std::endl;

    test_basic();
    test_lru();
    test_large();
    test_benchmark();

    std::cout << "\n=== All tests completed ===" << std::endl;
    return 0;
}
