// Test cache
#include <iostream>
#include "zerokv/cache.h"

using namespace zerokv;

void test_lru_cache() {
    std::cout << "\n=== Test: LRU Cache ===" << std::endl;

    LRUCache cache(3);

    cache.set("a", "1");
    cache.set("b", "2");
    cache.set("c", "3");

    std::string val;
    bool found = cache.get("a", &val);
    std::cout << "[Get a] " << (found ? val : "NOT FOUND") << std::endl;

    // Add one more, should evict b
    cache.set("d", "4");

    found = cache.get("b", &val);
    std::cout << "[Get b after eviction] " << (found ? val : "NOT FOUND (evicted)") << std::endl;

    found = cache.get("a", &val);
    std::cout << "[Get a] " << (found ? val : "NOT FOUND") << std::endl;

    auto stats = cache.stats();
    std::cout << "[Stats] hits=" << stats.hits << ", misses=" << stats.misses
              << ", hit_rate=" << (stats.hit_rate() * 100) << "%" << std::endl;
}

void test_cache_factory() {
    std::cout << "\n=== Test: Cache Factory ===" << std::endl;

    auto cache = create_cache(EvictionPolicy::LRU, 100);

    cache->set("key1", "value1");
    cache->set("key2", "value2");

    std::string val;
    if (cache->get("key1", &val)) {
        std::cout << "[Get key1] " << val << std::endl;
    }

    auto stats = cache->stats();
    std::cout << "[Stats] insertions=" << stats.insertions << ", hits=" << stats.hits << std::endl;
}

void test_cache_operations() {
    std::cout << "\n=== Test: Cache Operations ===" << std::endl;

    LRUCache cache(10);

    // Set
    cache.set("k1", "v1");
    cache.set("k2", "v2");

    // Exists
    std::cout << "[Exists k1] " << (cache.exists("k1") ? "yes" : "no") << std::endl;
    std::cout << "[Exists k3] " << (cache.exists("k3") ? "yes" : "no") << std::endl;

    // Delete
    cache.del("k1");
    std::cout << "[Exists k1 after del] " << (cache.exists("k1") ? "yes" : "no") << std::endl;

    // Clear
    cache.clear();
    std::cout << "[Exists k2 after clear] " << (cache.exists("k2") ? "yes" : "no") << std::endl;
}

void test_lfu_cache() {
    std::cout << "\n=== Test: LFU Cache ===" << std::endl;

    LFUCache cache(3);

    cache.set("a", "1");
    cache.set("b", "2");
    cache.set("c", "3");

    // Access "a" multiple times to increase its frequency
    std::string val;
    cache.get("a", &val);
    cache.get("a", &val);
    cache.get("a", &val);

    // Add one more, should evict "b" (lowest frequency)
    cache.set("d", "4");

    bool found = cache.get("b", &val);
    std::cout << "[Get b after eviction] " << (found ? val : "NOT FOUND (evicted)") << std::endl;

    found = cache.get("a", &val);
    std::cout << "[Get a] " << (found ? val : "NOT FOUND") << std::endl;

    auto stats = cache.stats();
    std::cout << "[Stats] hits=" << stats.hits << ", misses=" << stats.misses
              << ", evictions=" << stats.evictions << std::endl;
}

void test_fifo_cache() {
    std::cout << "\n=== Test: FIFO Cache ===" << std::endl;

    FIFOCache cache(3);

    cache.set("a", "1");
    cache.set("b", "2");
    cache.set("c", "3");

    // Access "a" (should not affect eviction order)
    std::string val;
    cache.get("a", &val);

    // Add one more, should evict "a" (first in)
    cache.set("d", "4");

    bool found = cache.get("a", &val);
    std::cout << "[Get a after eviction] " << (found ? val : "NOT FOUND (evicted)") << std::endl;

    found = cache.get("b", &val);
    std::cout << "[Get b] " << (found ? val : "NOT FOUND") << std::endl;

    auto stats = cache.stats();
    std::cout << "[Stats] hits=" << stats.hits << ", misses=" << stats.misses
              << ", evictions=" << stats.evictions << std::endl;
}

void test_random_cache() {
    std::cout << "\n=== Test: Random Cache ===" << std::endl;

    RandomCache cache(3);

    cache.set("a", "1");
    cache.set("b", "2");
    cache.set("c", "3");

    // Add one more, should evict randomly
    cache.set("d", "4");

    // Check what's left
    std::string val;
    bool found_a = cache.get("a", &val);
    bool found_b = cache.get("b", &val);
    bool found_c = cache.get("c", &val);
    bool found_d = cache.get("d", &val);

    std::cout << "[Get a] " << (found_a ? val : "NOT FOUND") << std::endl;
    std::cout << "[Get b] " << (found_b ? val : "NOT FOUND") << std::endl;
    std::cout << "[Get c] " << (found_c ? val : "NOT FOUND") << std::endl;
    std::cout << "[Get d] " << (found_d ? val : "NOT FOUND") << std::endl;

    auto stats = cache.stats();
    std::cout << "[Stats] insertions=" << stats.insertions << ", evictions=" << stats.evictions << std::endl;
}

void test_cache_factory_all_policies() {
    std::cout << "\n=== Test: Cache Factory All Policies ===" << std::endl;

    auto lru = create_cache(EvictionPolicy::LRU, 10);
    auto lfu = create_cache(EvictionPolicy::LFU, 10);
    auto fifo = create_cache(EvictionPolicy::FIFO, 10);
    auto random = create_cache(EvictionPolicy::RANDOM, 10);

    lru->set("k", "v");
    lfu->set("k", "v");
    fifo->set("k", "v");
    random->set("k", "v");

    std::string val;
    std::cout << "[LRU get] " << (lru->get("k", &val) ? val : "fail") << std::endl;
    std::cout << "[LFU get] " << (lfu->get("k", &val) ? val : "fail") << std::endl;
    std::cout << "[FIFO get] " << (fifo->get("k", &val) ? val : "fail") << std::endl;
    std::cout << "[Random get] " << (random->get("k", &val) ? val : "fail") << std::endl;
}

int main() {
    std::cout << "╔═══════════════════════════════════════╗" << std::endl;
    std::cout << "║  ZeroKV Cache Test                 ║" << std::endl;
    std::cout << "╚═══════════════════════════════════════╝" << std::endl;

    test_lru_cache();
    test_cache_factory();
    test_cache_operations();
    test_lfu_cache();
    test_fifo_cache();
    test_random_cache();
    test_cache_factory_all_policies();

    std::cout << "\n=== All cache tests completed ===" << std::endl;
    return 0;
}
