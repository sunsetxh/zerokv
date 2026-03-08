#include <gtest/gtest.h>
#include "p2p/config.h"
#include "p2p/worker.h"
#include "p2p/endpoint.h"

#include <thread>
#include <chrono>
#include <vector>
#include <cstring>
#include <atomic>

using namespace p2p;

// =============================================================================
// Loopback Communication Tests (single process, dual worker)
// =============================================================================

class LoopbackTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_ = Config::builder()
            .set_transport("tcp")
            .build();
        
        context_ = Context::create(config_);
        if (!context_) {
            GTEST_SKIP() << "UCX context creation failed";
        }
    }
    
    Config config_;
    Context::Ptr context_;
};

// Test basic worker creation
TEST_F(LoopbackTest, WorkerCreation) {
    auto worker = Worker::create(context_);
    ASSERT_NE(worker, nullptr);
}

// Test worker address retrieval
TEST_F(LoopbackTest, WorkerAddress) {
    auto worker = Worker::create(context_);
    auto addr = worker->address();
    EXPECT_GT(addr.size(), 0u) << "Worker address should not be empty";
}

// Test listener creation
TEST_F(LoopbackTest, ListenerCreation) {
    auto worker = Worker::create(context_);
    
    std::atomic<bool> accepted{false};
    auto listener = worker->listen("0.0.0.0:0", [&](Endpoint::Ptr ep) {
        (void)ep; accepted = true;
    });
    
    ASSERT_NE(listener, nullptr);
    EXPECT_FALSE(listener->address().empty());
    
    listener->close();
}

// Test that connect(address_blob) works
TEST_F(LoopbackTest, ConnectAddressBlob) {
    auto worker1 = Worker::create(context_);
    auto worker2 = Worker::create(context_);
    
    // Get worker2's address
    auto addr2 = worker2->address();
    EXPECT_GT(addr2.size(), 0u);
    
    // Connect worker1 to worker2 using address blob
    auto connect_future = worker1->connect(addr2);
    
    // Progress to complete connection
    for (int i = 0; i < 100 && !connect_future.ready(); ++i) {
        worker1->progress();
        worker2->progress();
    }
    
    // Should succeed
    EXPECT_TRUE(connect_future.ready());
    EXPECT_TRUE(connect_future.status().ok()) << "Status: " << connect_future.status().message();
    
    auto ep = connect_future.get();
    EXPECT_NE(ep, nullptr);
}

// Test worker-level tag_recv can be posted
TEST_F(LoopbackTest, TagRecvCanBePosted) {
    auto worker = Worker::create(context_);

    std::vector<char> buffer(1024, 0);
    auto future = worker->tag_recv(buffer.data(), buffer.size(), 100);

    // Should return a valid future (even though no data will arrive)
    EXPECT_NE(future.request(), nullptr);
}
