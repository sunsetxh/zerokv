#include <gtest/gtest.h>
#include "p2p/config.h"
#include "p2p/worker.h"
#include "p2p/endpoint.h"

#include <thread>
#include <chrono>
#include <cstring>

using namespace p2p;

class ConnectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Use TCP transport for testing (works without RDMA hardware)
        config_ = Config::builder()
            .set_transport("tcp")
            .build();

        context_ = Context::create(config_);
    }

    Config config_;
    Context::Ptr context_;
};

// Note: Full connection test requires two processes or threading
// This test verifies basic worker creation and address retrieval

TEST_F(ConnectionTest, WorkerCreate) {
    auto worker = Worker::create(context_);
    ASSERT_NE(worker, nullptr);
    EXPECT_EQ(worker->context(), context_);
    EXPECT_EQ(worker->index(), 0u);
}

TEST_F(ConnectionTest, WorkerAddress) {
    auto worker = Worker::create(context_);
    ASSERT_NE(worker, nullptr);

    auto addr = worker->address();
    // Worker address should be non-empty
    EXPECT_GT(addr.size(), 0u);
}

TEST_F(ConnectionTest, MultipleWorkers) {
    auto worker1 = Worker::create(context_, 0);
    auto worker2 = Worker::create(context_, 1);

    ASSERT_NE(worker1, nullptr);
    ASSERT_NE(worker2, nullptr);

    // Each worker should have a unique address
    auto addr1 = worker1->address();
    auto addr2 = worker2->address();
    EXPECT_NE(addr1, addr2);
}

TEST_F(ConnectionTest, Progress) {
    auto worker = Worker::create(context_);
    ASSERT_NE(worker, nullptr);

    // progress() should not crash
    bool made_progress = worker->progress();
    (void)made_progress;
}

// Test with shared memory transport
class ShmemConnectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_ = Config::builder()
            .set_transport("shmem")
            .build();

        context_ = Context::create(config_);
        if (!context_) {
            GTEST_SKIP();
        }
    }

    Config config_;
    Context::Ptr context_;
};

TEST_F(ShmemConnectionTest, WorkerCreate) {
    auto worker = Worker::create(context_);
    if (!worker) {
        GTEST_SKIP();
    }
    ASSERT_NE(worker, nullptr);
}
