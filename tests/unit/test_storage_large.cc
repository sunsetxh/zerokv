// Large value test cases
#include <gtest/gtest.h>
#include "zerokv/storage.h"
#include <vector>

using namespace zerokv;

// Test 1GB value support
TEST(LargeValueTest, OneGBValue) {
    StorageEngine storage(2UL * 1024 * 1024 * 1024); // 2GB

    std::string key = "large_1gb";
    std::vector<char> value(1024 * 1024 * 1024, 'L'); // 1GB

    Status status = storage.put(key, value.data(), value.size());
    EXPECT_EQ(status, Status::OK);

    std::vector<char> buffer(value.size());
    size_t size = buffer.size();
    status = storage.get(key, buffer.data(), &size);
    EXPECT_EQ(status, Status::OK);
}

// Test 512MB value
TEST(LargeValueTest, HalfGBValue) {
    StorageEngine storage(1024ULL * 1024 * 1024); // 1GB

    std::string key = "large_512mb";
    std::vector<char> value(512 * 1024 * 1024, 'M');

    Status status = storage.put(key, value.data(), value.size());
    EXPECT_EQ(status, Status::OK);
}

// Test chunked storage
TEST(LargeValueTest, ChunkedStorage) {
    StorageEngine storage(10ULL * 1024 * 1024); // 10MB

    // Multiple large values
    for (int i = 0; i < 5; ++i) {
        std::string key = "large_key_" + std::to_string(i);
        std::vector<char> value(2 * 1024 * 1024, 'A' + i); // 2MB each

        Status status = storage.put(key, value.data(), value.size());
        EXPECT_EQ(status, Status::OK);
    }
}
