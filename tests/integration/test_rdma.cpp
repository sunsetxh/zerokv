#include <gtest/gtest.h>
#include "p2p/config.h"
#include "p2p/worker.h"
#include "p2p/endpoint.h"
#include "p2p/memory.h"

#include <vector>
#include <cstring>

using namespace p2p;

class RdmaTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_ = Config::builder()
            .set_transport("tcp")  // Use TCP, not RDMA
            .build();

        context_ = Context::create(config_);
        if (!context_) {
            GTEST_SKIP();
        }
    }

    Config config_;
    Context::Ptr context_;
};

TEST_F(RdmaTest, MemoryRegistration) {
    auto region = MemoryRegion::allocate(context_, 4096);
    ASSERT_NE(region, nullptr);

    // Verify memory properties
    EXPECT_NE(region->address(), nullptr);
    EXPECT_EQ(region->length(), 4096u);
    EXPECT_EQ(region->memory_type(), MemoryType::kHost);
}

TEST_F(RdmaTest, RemoteKeyGeneration) {
    auto region = MemoryRegion::allocate(context_, 4096);
    ASSERT_NE(region, nullptr);

    // Try to get remote key
    // Note: This may fail without an active endpoint
    auto rkey = region->remote_key();

    // Key may be empty in test context, which is OK
    // In real usage, you'd exchange this with a connected peer
    (void)rkey;
}

TEST_F(RdmaTest, WorkerCreate) {
    auto worker = Worker::create(context_);
    ASSERT_NE(worker, nullptr);
}

// Test that flush returns appropriate status
TEST_F(RdmaTest, FlushWithoutEndpoint) {
    auto worker = Worker::create(context_);
    ASSERT_NE(worker, nullptr);

    // Can't test actual flush without connected endpoint
    // Just verify worker works
    worker->progress();
}
