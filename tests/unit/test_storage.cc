#include <gtest/gtest.h>
#include "zerokv/storage.h"
#include <string>
#include <cstring>
#include <vector>
#include <random>

using namespace zerokv;

// ==================== Basic Tests ====================

TEST(StorageTest, PutAndGet) {
    StorageEngine storage(1024 * 1024); // 1MB

    std::string key = "test_key";
    std::string value = "test_value";

    Status status = storage.put(key, value.data(), value.size());
    EXPECT_EQ(status, Status::OK);

    char buffer[1024];
    size_t size = sizeof(buffer);
    status = storage.get(key, buffer, &size);

    EXPECT_EQ(status, Status::OK);
    EXPECT_EQ(size, value.size());
    EXPECT_EQ(std::strncmp(buffer, value.data(), size), 0);
}

TEST(StorageTest, GetNonExistent) {
    StorageEngine storage(1024 * 1024);

    char buffer[1024];
    size_t size = sizeof(buffer);
    Status status = storage.get("nonexistent", buffer, &size);

    EXPECT_EQ(status, Status::NOT_FOUND);
}

TEST(StorageTest, Delete) {
    StorageEngine storage(1024 * 1024);

    std::string key = "test_key";
    std::string value = "test_value";

    storage.put(key, value.data(), value.size());

    Status status = storage.delete_key(key);
    EXPECT_EQ(status, Status::OK);

    char buffer[1024];
    size_t size = sizeof(buffer);
    status = storage.get(key, buffer, &size);
    EXPECT_EQ(status, Status::NOT_FOUND);
}

TEST(StorageTest, UpdateExistingKey) {
    StorageEngine storage(1024 * 1024);

    std::string key = "test_key";
    std::string value1 = "value1";
    std::string value2 = "value2";

    storage.put(key, value1.data(), value1.size());

    char buffer[1024];
    size_t size = sizeof(buffer);
    storage.get(key, buffer, &size);
    EXPECT_EQ(std::strncmp(buffer, value1.data(), size), 0);

    // Update with new value
    storage.put(key, value2.data(), value2.size());

    size = sizeof(buffer);
    storage.get(key, buffer, &size);
    EXPECT_EQ(std::strncmp(buffer, value2.data(), size), 0);
}

// ==================== LRU Tests ====================

TEST(StorageTest, LRUEviction) {
    StorageEngine storage(100); // Very small for testing

    // Put multiple items to trigger eviction
    for (int i = 0; i < 20; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string value(50, 'x'); // 50 bytes each
        storage.put(key, value.data(), value.size());
    }

    // First items should be evicted
    char buffer[1024];
    size_t size = sizeof(buffer);
    Status status = storage.get("key_0", buffer, &size);
    EXPECT_EQ(status, Status::NOT_FOUND);

    // Recent items should exist
    status = storage.get("key_19", buffer, &size);
    EXPECT_EQ(status, Status::OK);
}

TEST(StorageTest, LRUAccessOrder) {
    StorageEngine storage(200);

    // Put items
    for (int i = 0; i < 5; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string value(30, 'x' + i);
        storage.put(key, value.data(), value.size());
    }

    // Access key_0 to update LRU
    char buffer[1024];
    size_t size = sizeof(buffer);
    storage.get("key_0", buffer, &size);

    // Put more items to trigger eviction
    for (int i = 5; i < 10; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string value(30, 'x' + i);
        storage.put(key, value.data(), value.size());
    }

    // key_0 should still exist (was accessed recently)
    size = sizeof(buffer);
    Status status = storage.get("key_0", buffer, &size);
    EXPECT_EQ(status, Status::OK);

    // key_1 should be evicted
    size = sizeof(buffer);
    status = storage.get("key_1", buffer, &size);
    EXPECT_EQ(status, Status::NOT_FOUND);
}

// ==================== Memory Tests ====================

TEST(StorageTest, OutOfMemory) {
    StorageEngine storage(100); // Very small

    std::string key = "big_key";
    std::string value(200, 'x'); // Bigger than storage

    Status status = storage.put(key, value.data(), value.size());
    EXPECT_EQ(status, Status::OUT_OF_MEMORY);
}

TEST(StorageTest, LargeValue) {
    StorageEngine storage(1024 * 1024); // 1MB

    std::string key = "large_key";
    std::string value(512 * 1024, 'L'); // 512KB

    Status status = storage.put(key, value.data(), value.size());
    EXPECT_EQ(status, Status::OK);

    char* buffer = new char[value.size()];
    size_t size = value.size();
    status = storage.get(key, buffer, &size);

    EXPECT_EQ(status, Status::OK);
    EXPECT_EQ(size, value.size());

    delete[] buffer;
}

TEST(StorageTest, ZeroByteValue) {
    StorageEngine storage(1024 * 1024);

    std::string key = "empty_key";
    std::string value = "";

    Status status = storage.put(key, value.data(), value.size());
    EXPECT_EQ(status, Status::OK);

    char buffer[1024];
    size_t size = sizeof(buffer);
    status = storage.get(key, buffer, &size);

    EXPECT_EQ(status, Status::OK);
    EXPECT_EQ(size, 0);
}

// ==================== Stress Tests ====================

TEST(StorageStressTest, ManyKeys) {
    StorageEngine storage(10 * 1024 * 1024); // 10MB

    const int num_keys = 10000;

    // Insert many keys
    for (int i = 0; i < num_keys; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string value = "value_" + std::to_string(i);
        Status status = storage.put(key, value.data(), value.size());
        EXPECT_EQ(status, Status::OK);
    }

    EXPECT_EQ(storage.item_count(), num_keys);

    // Verify some keys
    for (int i = 0; i < 100; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string expected = "value_" + std::to_string(i);

        char buffer[1024];
        size_t size = sizeof(buffer);
        Status status = storage.get(key, buffer, &size);

        EXPECT_EQ(status, Status::OK);
        EXPECT_EQ(size, expected.size());
    }
}

TEST(StorageStressTest, RandomAccess) {
    StorageEngine storage(10 * 1024 * 1024);

    const int num_keys = 1000;
    std::vector<std::string> keys;
    keys.reserve(num_keys);

    // Insert random data
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> size_dist(10, 1000);

    for (int i = 0; i < num_keys; ++i) {
        std::string key = "key_" + std::to_string(i);
        int size = size_dist(gen);
        std::string value(size, 'a' + (i % 26));

        storage.put(key, value.data(), value.size());
        keys.push_back(key);
    }

    // Random access
    std::uniform_int_distribution<> idx_dist(0, num_keys - 1);
    for (int i = 0; i < 1000; ++i) {
        int idx = idx_dist(gen);
        char buffer[1024];
        size_t size = sizeof(buffer);
        storage.get(keys[idx], buffer, &size);
    }
}

// ==================== Stats Tests ====================

TEST(StorageTest, Stats) {
    StorageEngine storage(1024 * 1024);

    EXPECT_EQ(storage.used_memory(), 0u);
    EXPECT_EQ(storage.item_count(), 0u);

    std::string key1 = "key1";
    std::string value1 = "value1";
    storage.put(key1, value1.data(), value1.size());

    EXPECT_GT(storage.used_memory(), 0u);
    EXPECT_EQ(storage.item_count(), 1u);

    std::string key2 = "key2";
    std::string value2 = "value2";
    storage.put(key2, value2.data(), value2.size());

    EXPECT_EQ(storage.item_count(), 2u);

    storage.delete_key(key1);
    EXPECT_EQ(storage.item_count(), 1u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
