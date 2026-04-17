#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace zerokv::examples::alps_kv_bench {
std::vector<size_t> parse_sizes_csv(const std::string& csv);
size_t max_size_bytes_for_sizes(const std::vector<size_t>& sizes);
std::string render_round_summary(const char* role,
                                 size_t round,
                                 size_t size,
                                 int iters,
                                 size_t total_bytes,
                                 uint64_t elapsed_us);
std::string render_listen_address_line(const std::string& address);
}  // namespace zerokv::examples::alps_kv_bench

TEST(AlpsKvBenchHelpersTest, ParsesSizeCsvWithBinarySuffixes) {
    const auto sizes = zerokv::examples::alps_kv_bench::parse_sizes_csv("1M, 4M,16M");
    ASSERT_EQ(sizes.size(), 3u);
    EXPECT_EQ(sizes[0], 1024u * 1024u);
    EXPECT_EQ(sizes[1], 4u * 1024u * 1024u);
    EXPECT_EQ(sizes[2], 16u * 1024u * 1024u);
}

TEST(AlpsKvBenchHelpersTest, ParsesExpandedDefaultSweep) {
    const auto sizes = zerokv::examples::alps_kv_bench::parse_sizes_csv(
        "256K,512K,1M,2M,4M,8M,16M,32M,64M");
    ASSERT_EQ(sizes.size(), 9u);
    EXPECT_EQ(sizes.front(), 256u * 1024u);
    EXPECT_EQ(sizes.back(), 64u * 1024u * 1024u);
}

TEST(AlpsKvBenchHelpersTest, MaxSizeUsesLargestConfiguredRound) {
    const std::vector<size_t> sizes = {1024u, 8u * 1024u, 1024u * 1024u};
    EXPECT_EQ(zerokv::examples::alps_kv_bench::max_size_bytes_for_sizes(sizes), 1024u * 1024u);
}

TEST(AlpsKvBenchHelpersTest, RoundSummaryIncludesRoundAndSize) {
    const auto line = zerokv::examples::alps_kv_bench::render_round_summary(
        "client", 2, 4u * 1024u * 1024u, 10, 40u * 1024u * 1024u, 12345);
    EXPECT_NE(line.find("ALPS_KV_ROUND"), std::string::npos);
    EXPECT_NE(line.find("role=client"), std::string::npos);
    EXPECT_NE(line.find("round=2"), std::string::npos);
    EXPECT_NE(line.find("size=4194304"), std::string::npos);
    EXPECT_NE(line.find("iters=10"), std::string::npos);
}

TEST(AlpsKvBenchHelpersTest, ListenAddressLineIncludesResolvedAddress) {
    const auto line = zerokv::examples::alps_kv_bench::render_listen_address_line("10.0.2.15:16000");
    EXPECT_NE(line.find("ALPS_KV_LISTEN"), std::string::npos);
    EXPECT_NE(line.find("address=10.0.2.15:16000"), std::string::npos);
}
