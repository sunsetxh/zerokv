#include <gtest/gtest.h>
#include "zerokv/future.h"
#include "zerokv/endpoint.h"

using namespace axon;

TEST(FutureVoidTest, Ready) {
    auto f = Future<void>::make_ready();
    EXPECT_TRUE(f.ready());
}

TEST(FutureVoidTest, Get) {
    auto f = Future<void>::make_ready();
    EXPECT_NO_THROW(f.get());
    EXPECT_TRUE(f.status().ok());
}

TEST(FutureVoidTest, MakeError) {
    auto f = Future<void>::make_error(Status(ErrorCode::kInvalidArgument, "test error"));
    EXPECT_TRUE(f.ready());
    EXPECT_FALSE(f.status().ok());
    EXPECT_EQ(f.status().code(), ErrorCode::kInvalidArgument);
}

TEST(FutureSizeTTest, Ready) {
    auto f = Future<size_t>::make_ready(42);
    EXPECT_TRUE(f.ready());
}

TEST(FutureSizeTTest, Get) {
    auto f = Future<size_t>::make_ready(42);
    EXPECT_EQ(f.get(), 42);
}

TEST(FutureSizeTTest, MakeError) {
    auto f = Future<size_t>::make_error(Status(ErrorCode::kOutOfMemory, "test"));
    EXPECT_TRUE(f.ready());
    EXPECT_FALSE(f.status().ok());
}

TEST(FuturePairTest, Ready) {
    auto f = Future<std::pair<size_t, Tag>>::make_ready({100, 42});
    EXPECT_TRUE(f.ready());
}

TEST(FuturePairTest, Get) {
    auto f = Future<std::pair<size_t, Tag>>::make_ready({100, 42});
    auto [bytes, tag] = f.get();
    EXPECT_EQ(bytes, 100u);
    EXPECT_EQ(tag, 42u);
}
