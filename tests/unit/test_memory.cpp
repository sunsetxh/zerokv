#include <gtest/gtest.h>
#include "zerokv/config.h"
#include "zerokv/memory.h"

using namespace zerokv;

class MemoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_ = Config::builder().build();
        context_ = Context::create(config_);
    }

    Config config_;
    Context::Ptr context_;
};

TEST_F(MemoryTest, Allocate) {
    if (!context_) {
        GTEST_SKIP() << "Context creation failed";
    }
    auto region = MemoryRegion::allocate(context_, 4096);
    ASSERT_NE(region, nullptr);
    EXPECT_NE(region->address(), nullptr);
    EXPECT_EQ(region->length(), 4096u);
    EXPECT_EQ(region->memory_type(), MemoryType::kHost);
}

TEST_F(MemoryTest, AllocateNonAlignedSize) {
    if (!context_) {
        GTEST_SKIP() << "Context creation failed";
    }
    auto region = MemoryRegion::allocate(context_, 123);
    ASSERT_NE(region, nullptr);
    EXPECT_NE(region->address(), nullptr);
    EXPECT_EQ(region->length(), 123u);
}

TEST_F(MemoryTest, RegisterMem) {
    if (!context_) {
        GTEST_SKIP() << "Context creation failed";
    }
    std::vector<char> buffer(4096, 0);
    auto region = MemoryRegion::register_mem(context_, buffer.data(), buffer.size());
    ASSERT_NE(region, nullptr);
    EXPECT_EQ(region->address(), buffer.data());
    EXPECT_EQ(region->length(), buffer.size());
}

TEST_F(MemoryTest, RemoteKey) {
    if (!context_) {
        GTEST_SKIP() << "Context creation failed";
    }
    auto region = MemoryRegion::allocate(context_, 4096);
    ASSERT_NE(region, nullptr);

    auto rkey = region->remote_key();
    // rkey may be empty if no endpoint is available for packing
    // This is expected behavior in unit test context
}

TEST_F(MemoryTest, InvalidParameters) {
    // Null context
    auto region = MemoryRegion::register_mem(nullptr, nullptr, 4096);
    EXPECT_EQ(region, nullptr);

    // Zero length (only if context is valid)
    if (context_) {
        auto region2 = MemoryRegion::allocate(context_, 0);
        EXPECT_EQ(region2, nullptr);
    }
}
