// Fixed storage engine v2
#include <iostream>
#include <chrono>
#include <cstring>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <list>
#include <mutex>

class StorageEngine {
public:
    static constexpr size_t CHUNK_SIZE = 64 * 1024;

    struct Chunk {
        uint8_t data[CHUNK_SIZE];
        bool in_use = false;
    };

    StorageEngine(size_t max_memory) : max_memory_(max_memory), used_memory_(0) {
        chunk_count_ = max_memory / CHUNK_SIZE;
        chunks_.resize(chunk_count_);
        std::cout << "[Storage] Init: " << chunk_count_ << " chunks, "
                  << (max_memory / 1024 / 1024) << "MB" << std::endl;
    }

    bool put(const std::string& key, const void* value, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Free existing key
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            free_chunks(it->second.chunks);
            used_memory_ -= it->second.size;
            lru_list_.erase(it->second.lru_iter);
            cache_.erase(it);
        }

        // Evict LRU until have enough chunks AND memory
        while (!lru_list_.empty()) {
            // Check if we have both enough memory AND enough chunks
            bool have_enough = (used_memory_ + size <= max_memory_);
            size_t needed = (size + CHUNK_SIZE - 1) / CHUNK_SIZE;
            size_t available_chunks = 0;
            for (size_t i = 0; i < chunk_count_; i++) {
                if (!chunks_[i].in_use) available_chunks++;
            }
            have_enough = have_enough && (available_chunks >= needed);

            if (have_enough) break;

            // Evict one LRU item
            std::string evict_key = lru_list_.front();
            lru_list_.pop_front();

            auto eit = cache_.find(evict_key);
            if (eit != cache_.end()) {
                free_chunks(eit->second.chunks);
                used_memory_ -= eit->second.size;
                cache_.erase(eit);
            }
        }

        // Allocate chunks
        size_t needed = (size + CHUNK_SIZE - 1) / CHUNK_SIZE;
        std::vector<int> chunk_idx;

        for (size_t i = 0; i < chunk_count_ && chunk_idx.size() < needed; i++) {
            if (!chunks_[i].in_use) {
                chunks_[i].in_use = true;
                chunk_idx.push_back(i);
            }
        }

        if (chunk_idx.size() < needed) {
            std::cerr << "[ERROR] OOM: need " << needed << " chunks" << std::endl;
            return false;
        }

        // Copy data
        const uint8_t* src = static_cast<const uint8_t*>(value);
        size_t offset = 0;
        for (int idx : chunk_idx) {
            size_t copy = std::min(size - offset, CHUNK_SIZE);
            memcpy(chunks_[idx].data, src + offset, copy);
            offset += copy;
        }

        // Store
        CacheEntry entry;
        entry.chunks = std::move(chunk_idx);
        entry.size = size;
        entry.lru_iter = lru_list_.insert(lru_list_.end(), key);
        cache_[key] = std::move(entry);
        used_memory_ += size;

        return true;
    }

    bool get(const std::string& key, void* buffer, size_t* size) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = cache_.find(key);
        if (it == cache_.end()) return false;

        if (*size < it->second.size) {
            *size = it->second.size;
            return false;
        }

        // Move to front of LRU
        lru_list_.erase(it->second.lru_iter);
        it->second.lru_iter = lru_list_.insert(lru_list_.end(), key);

        // Copy data
        uint8_t* dst = static_cast<uint8_t*>(buffer);
        size_t offset = 0;
        for (int idx : it->second.chunks) {
            size_t copy = std::min(it->second.size - offset, CHUNK_SIZE);
            memcpy(dst + offset, chunks_[idx].data, copy);
            offset += copy;
        }

        *size = it->second.size;
        return true;
    }

    bool remove(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = cache_.find(key);
        if (it == cache_.end()) return false;

        free_chunks(it->second.chunks);
        used_memory_ -= it->second.size;
        lru_list_.erase(it->second.lru_iter);
        cache_.erase(it);

        return true;
    }

    size_t used_memory() const { return used_memory_; }
    size_t item_count() const { return cache_.size(); }

private:
    struct CacheEntry {
        std::vector<int> chunks;
        size_t size;
        std::list<std::string>::iterator lru_iter;
    };

    void free_chunks(const std::vector<int>& idxs) {
        for (int idx : idxs) {
            chunks_[idx].in_use = false;
        }
    }

    std::vector<Chunk> chunks_;
    size_t chunk_count_;
    std::unordered_map<std::string, CacheEntry> cache_;
    std::list<std::string> lru_list_;
    size_t max_memory_;
    size_t used_memory_;
    std::mutex mutex_;
};

void test_basic() {
    std::cout << "\n=== Test 1: Basic Put/Get ===" << std::endl;
    StorageEngine s(1024 * 1024);

    s.put("key", "Hello", 5);
    char buf[100];
    size_t sz = sizeof(buf);
    bool ok = s.get("key", buf, &sz);
    buf[sz] = '\0';
    std::cout << "[Result] " << (ok ? buf : "FAILED") << std::endl;
}

void test_lru() {
    std::cout << "\n=== Test 2: LRU ===" << std::endl;
    StorageEngine s(10 * 1024 * 1024); // 10MB

    // Insert 100 items
    for (int i = 0; i < 100; i++) {
        std::string key = "k" + std::to_string(i);
        std::string val(1000, 'x');
        s.put(key, val.data(), val.size());
    }
    std::cout << "[Inserted] " << s.item_count() << " items, "
              << (s.used_memory() / 1024) << "KB used" << std::endl;

    // First should be evicted
    char buf[2000];
    size_t sz = sizeof(buf);
    bool ok = s.get("k0", buf, &sz);
    std::cout << "[k0] " << (ok ? "FOUND" : "EVICTED") << std::endl;

    ok = s.get("k99", buf, &sz);
    std::cout << "[k99] " << (ok ? "FOUND" : "NOT FOUND") << std::endl;
}

void test_large() {
    std::cout << "\n=== Test 3: Large Value ===" << std::endl;
    StorageEngine s(10 * 1024 * 1024);

    std::string val(1024 * 1024, 'L');
    auto t1 = std::chrono::high_resolution_clock::now();
    bool ok = s.put("large", val.data(), val.size());
    auto t2 = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
    std::cout << "[Put 1MB] " << (ok ? "OK" : "FAIL") << " (" << ms << " us)" << std::endl;
}

void test_bench() {
    std::cout << "\n=== Test 4: Benchmark ===" << std::endl;
    StorageEngine s(50 * 1024 * 1024);

    const int N = 5000;
    std::string val(1024, 'x');

    auto t1 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N; i++) {
        s.put("k" + std::to_string(i), val.data(), val.size());
    }
    auto t2 = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();

    std::cout << "[Put " << N << " x 1KB] " << ms << " ms" << std::endl;
    std::cout << "[Throughput] " << (N * 1000 / ms) << " ops/sec" << std::endl;
    std::cout << "[Used] " << (s.used_memory() / 1024) << " KB" << std::endl;
}

int main() {
    std::cout << "╔═══════════════════════════════════╗" << std::endl;
    std::cout << "║  ZeroKV Storage Test (Fixed v2) ║" << std::endl;
    std::cout << "╚═══════════════════════════════════╝" << std::endl;

    test_basic();
    test_lru();
    test_large();
    test_bench();

    std::cout << "\n=== DONE ===" << std::endl;
    return 0;
}
