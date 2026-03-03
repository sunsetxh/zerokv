// Standalone storage test without UCX dependency
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
#include <thread>

// Simplified storage engine for testing
class StorageEngine {
public:
    static constexpr size_t CHUNK_SIZE = 64 * 1024;

    struct Chunk {
        uint8_t* data;
        size_t size;
        bool in_use;
        Chunk() : data(nullptr), size(0), in_use(false) {}
    };

    struct ValueHandle {
        std::vector<Chunk*> chunks;
        size_t total_size;
        ValueHandle() : total_size(0) {}
    };

    StorageEngine(size_t max_memory) : max_memory_(max_memory), used_memory_(0) {
        size_t chunk_count = max_memory / CHUNK_SIZE;
        memory_pool_.reserve(chunk_count);

        for (size_t i = 0; i < chunk_count; ++i) {
            auto chunk = std::make_unique<Chunk>();
            chunk->data = new uint8_t[CHUNK_SIZE];
            chunk->size = CHUNK_SIZE;
            chunk->in_use = false;
            memory_pool_.push_back(std::move(chunk));
        }
        std::cout << "[Storage] Initialized with " << chunk_count << " chunks, "
                  << (max_memory / 1024 / 1024) << "MB" << std::endl;
    }

    ~StorageEngine() {
        std::cout << "[Storage] Shutdown, used " << (used_memory_ / 1024 / 1024) << "MB" << std::endl;
    }

    bool put(const std::string& key, const void* value, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);

        // First try to evict old items if needed
        while (used_memory_ + size > max_memory_ && !lru_list_.empty()) {
            auto it = lru_list_.begin();
            auto cache_it = cache_.find(*it);
            if (cache_it != cache_.end()) {
                release_chunks(cache_it->second.value);
                used_memory_ -= cache_it->second.value.total_size;
                cache_.erase(cache_it);
            }
            lru_list_.erase(it);
        }

        // Allocate chunks
        std::vector<Chunk*> chunks;
        size_t needed = (size + CHUNK_SIZE - 1) / CHUNK_SIZE;

        for (auto& chunk : memory_pool_) {
            if (!chunk->in_use && chunks.size() < needed) {
                chunk->in_use = true;
                chunks.push_back(chunk.get());
            }
            if (chunks.size() >= needed) break;
        }

        if (chunks.empty()) {
            std::cerr << "[ERROR] Out of memory" << std::endl;
            return false;
        }

        // Copy data
        const uint8_t* src = static_cast<const uint8_t*>(value);
        size_t offset = 0;
        for (auto* chunk : chunks) {
            size_t copy_size = std::min(size - offset, CHUNK_SIZE);
            memcpy(chunk->data, src + offset, copy_size);
            offset += copy_size;
        }

        // Store
        ValueHandle handle;
        handle.chunks = std::move(chunks);
        handle.total_size = size;

        auto it = cache_.find(key);
        if (it != cache_.end()) {
            release_chunks(it->second.value);
            used_memory_ -= it->second.value.total_size;
            lru_list_.erase(it->second.lru_iter);
        }

        cache_[key] = {key, handle, lru_list_.insert(lru_list_.end(), key)};
        used_memory_ += size;

        return true;
    }

    bool get(const std::string& key, void* buffer, size_t* size) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = cache_.find(key);
        if (it == cache_.end()) {
            return false;
        }

        // Update LRU
        lru_list_.erase(it->second.lru_iter);
        it->second.lru_iter = lru_list_.insert(lru_list_.end(), key);

        if (*size < it->second.value.total_size) {
            *size = it->second.value.total_size;
            return false;
        }

        // Copy data
        uint8_t* dst = static_cast<uint8_t*>(buffer);
        size_t offset = 0;
        for (auto* chunk : it->second.value.chunks) {
            size_t copy_size = std::min(it->second.value.total_size - offset, CHUNK_SIZE);
            memcpy(dst + offset, chunk->data, copy_size);
            offset += copy_size;
        }

        *size = it->second.value.total_size;
        return true;
    }

    bool remove(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = cache_.find(key);
        if (it == cache_.end()) {
            return false;
        }

        release_chunks(it->second.value);
        used_memory_ -= it->second.value.total_size;
        lru_list_.erase(it->second.lru_iter);
        cache_.erase(it);

        return true;
    }

    size_t used_memory() const { return used_memory_; }
    size_t item_count() const { return cache_.size(); }

private:
    struct CacheEntry {
        std::string key;
        ValueHandle value;
        std::list<std::string>::iterator lru_iter;
    };

    void release_chunks(ValueHandle& handle) {
        for (auto* chunk : handle.chunks) {
            chunk->in_use = false;
        }
        handle.chunks.clear();
    }

    std::vector<std::unique_ptr<Chunk>> memory_pool_;
    std::list<std::string> lru_list_;
    std::unordered_map<std::string, CacheEntry> cache_;
    size_t max_memory_;
    size_t used_memory_;
    std::mutex mutex_;
};

void test_basic() {
    std::cout << "\n=== Test 1: Basic Put/Get ===" << std::endl;
    StorageEngine storage(1024 * 1024);

    std::string key = "test_key";
    std::string value = "Hello ZeroKV!";

    std::cout << "[Test] Put key=" << key << ", value_size=" << value.size() << std::endl;
    bool ok = storage.put(key, value.data(), value.size());
    std::cout << "[Result] " << (ok ? "SUCCESS" : "FAILED") << std::endl;

    char buffer[1024];
    size_t size = sizeof(buffer);
    ok = storage.get(key, buffer, &size);
    std::cout << "[Test] Get key=" << key << std::endl;
    std::cout << "[Result] " << (ok ? "SUCCESS" : "FAILED") << std::endl;
    if (ok) {
        buffer[size] = '\0';
        std::cout << "[Value] " << buffer << std::endl;
    }
}

void test_lru() {
    std::cout << "\n=== Test 2: LRU Eviction ===" << std::endl;
    StorageEngine storage(64 * 1024); // 64KB

    std::cout << "[Test] Insert 20 items (50 bytes each)" << std::endl;
    for (int i = 0; i < 20; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string value(50, 'x');
        storage.put(key, value.data(), value.size());
    }

    std::cout << "[Info] Items in storage: " << storage.item_count() << std::endl;

    // First item should be evicted
    char buffer[1024];
    size_t size = sizeof(buffer);
    bool ok = storage.get("key_0", buffer, &size);
    std::cout << "[Test] Get key_0: " << (ok ? "FOUND" : "NOT FOUND (evicted)") << std::endl;

    // Last item should exist
    ok = storage.get("key_19", buffer, &size);
    std::cout << "[Test] Get key_19: " << (ok ? "FOUND" : "NOT FOUND") << std::endl;
}

void test_large_value() {
    std::cout << "\n=== Test 3: Large Value (1MB) ===" << std::endl;
    StorageEngine storage(10 * 1024 * 1024); // 10MB

    std::string key = "large_key";
    std::string value(1024 * 1024, 'L'); // 1MB

    auto start = std::chrono::high_resolution_clock::now();

    bool ok = storage.put(key, value.data(), value.size());
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "[Result] " << (ok ? "SUCCESS" : "FAILED") << std::endl;
    std::cout << "[Perf] Put 1MB took " << duration.count() << " us" << std::endl;

    // Verify
    std::vector<char> buffer(value.size());
    size_t read_size = buffer.size();
    ok = storage.get(key, buffer.data(), &read_size);
    std::cout << "[Verify] " << (ok && read_size == value.size() ? "PASSED" : "FAILED") << std::endl;
}

void test_benchmark() {
    std::cout << "\n=== Test 4: Benchmark ===" << std::endl;
    StorageEngine storage(50 * 1024 * 1024); // 50MB

    const int num_ops = 1000;
    std::string value(1024, 'x');

    // Put benchmark
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_ops; ++i) {
        std::string key = "key_" + std::to_string(i);
        storage.put(key, value.data(), value.size());
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "[Perf] Put " << num_ops << " x 1KB: " << duration.count() << " ms" << std::endl;
    std::cout << "[Perf] Throughput: " << (num_ops * 1000 / duration.count()) << " ops/sec" << std::endl;
    std::cout << "[Info] Storage used: " << (storage.used_memory() / 1024) << " KB" << std::endl;

    // Get benchmark
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, num_ops - 1);

    std::vector<char> buffer(1024);
    size_t size = buffer.size();

    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_ops; ++i) {
        int idx = dist(gen);
        std::string key = "key_" + std::to_string(idx);
        storage.get(key, buffer.data(), &size);
    }
    end = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "[Perf] Get random " << num_ops << " times: " << duration.count() << " ms" << std::endl;
    if (duration.count() > 0) {
        std::cout << "[Perf] Throughput: " << (num_ops * 1000 / duration.count()) << " ops/sec" << std::endl;
    } else {
        std::cout << "[Perf] Throughput: N/A (too fast)" << std::endl;
    }
}

void test_delete() {
    std::cout << "\n=== Test 5: Delete ===" << std::endl;
    StorageEngine storage(1024 * 1024);

    // Put
    storage.put("to_delete", "value", 5);
    std::cout << "[Info] After put: " << storage.item_count() << " items" << std::endl;

    // Delete
    bool ok = storage.remove("to_delete");
    std::cout << "[Result] Delete: " << (ok ? "SUCCESS" : "FAILED") << std::endl;
    std::cout << "[Info] After delete: " << storage.item_count() << " items" << std::endl;

    // Verify deleted
    char buffer[1024];
    size_t size = sizeof(buffer);
    ok = storage.get("to_delete", buffer, &size);
    std::cout << "[Verify] Get after delete: " << (ok ? "FOUND" : "NOT FOUND (correct)") << std::endl;
}

int main() {
    std::cout << "╔════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║          ZeroKV Storage Engine Test Suite                 ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════╝" << std::endl;

    test_basic();
    test_lru();
    test_large_value();
    test_benchmark();
    test_delete();

    std::cout << "\n=== All tests completed ===" << std::endl;
    return 0;
}
