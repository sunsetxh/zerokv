#include "kv/bench_utils.h"

#include <gtest/gtest.h>

namespace zerokv::core::detail {

TEST(KvBenchUtilsTest, ParseSizeListSupportsBinarySuffixes) {
    const auto sizes = parse_size_list("4K,64K,1M,32M");
    ASSERT_TRUE(sizes.ok()) << sizes.status.message();
    ASSERT_EQ(sizes.value().size(), 4u);
    EXPECT_EQ(sizes.value()[0], 4ull * 1024ull);
    EXPECT_EQ(sizes.value()[1], 64ull * 1024ull);
    EXPECT_EQ(sizes.value()[2], 1ull * 1024ull * 1024ull);
    EXPECT_EQ(sizes.value()[3], 32ull * 1024ull * 1024ull);
}

TEST(KvBenchUtilsTest, ParseSizeListRejectsInvalidToken) {
    const auto sizes = parse_size_list("4K,boom,1M");
    EXPECT_FALSE(sizes.ok());
}

TEST(KvBenchUtilsTest, DeriveIterationsUsesTotalBytesAndMinOne) {
    EXPECT_EQ(derive_iterations(4ull * 1024ull, std::nullopt, 1ull << 30), 262144u);
    EXPECT_EQ(derive_iterations(128ull * 1024ull * 1024ull, std::nullopt, 1ull << 30), 8u);
    EXPECT_EQ(derive_iterations(2ull * 1024ull * 1024ull * 1024ull, std::nullopt, 1ull << 30), 1u);
}

TEST(KvBenchUtilsTest, DeriveIterationsPrefersExplicitIters) {
    EXPECT_EQ(derive_iterations(4ull * 1024ull, 7u, 1ull << 30), 7u);
}

}  // namespace zerokv::core::detail
