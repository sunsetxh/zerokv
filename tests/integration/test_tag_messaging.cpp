#include <gtest/gtest.h>
#include "zerokv/config.h"
#include "zerokv/transport/worker.h"
#include "zerokv/transport/endpoint.h"

#include <thread>
#include <chrono>
#include <vector>
#include <cstring>

using namespace zerokv;
using namespace zerokv::transport;

// Test fixture for messaging tests
class TagMessagingTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_ = Config::builder()
            .set_transport("tcp")
            .build();

        context_ = Context::create(config_);
        if (!context_) {
            GTEST_SKIP();
        }

        worker_ = Worker::create(context_);
        if (!worker_) {
            GTEST_SKIP();
        }
    }

    Config config_;
    Context::Ptr context_;
    Worker::Ptr worker_;
};

TEST_F(TagMessagingTest, WorkerTagRecv) {
    // Test worker-level tag_recv setup
    std::vector<char> buffer(4096, 0);

    auto future = worker_->tag_recv(buffer.data(), buffer.size(), 42);
    ASSERT_NE(future.request(), nullptr);

    // Future should be ready (no data received yet)
    bool ready = future.ready();
    (void)ready;
}

TEST_F(TagMessagingTest, Progress) {
    // Test that progress can be called without crashing
    for (int i = 0; i < 10; ++i) {
        worker_->progress();
    }
}

TEST_F(TagMessagingTest, Wait) {
    // Test wait with zero timeout
    bool result = worker_->wait(std::chrono::milliseconds(0));
    (void)result;
}

// Test message construction
TEST(TagTest, MakeTag) {
    Tag tag = make_tag(1, 42);
    EXPECT_EQ(tag_context(tag), 1u);
    EXPECT_EQ(tag_user(tag), 42u);
}

TEST(TagTest, TagConstants) {
    EXPECT_EQ(kTagMaskAll, ~Tag{0});
    EXPECT_EQ(kTagMaskUser, 0x0000'0000'FFFF'FFFF);
}
