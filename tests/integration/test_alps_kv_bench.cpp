#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace zerokv::examples::alps_kv_bench {
std::vector<size_t> parse_sizes_csv(const std::string& csv);
size_t max_size_bytes_for_sizes(const std::vector<size_t>& sizes);
struct RoundTimingSummary {
    uint64_t avg_control_request_grant_us = 0;
    uint64_t avg_put_us = 0;
    uint64_t avg_flush_us = 0;
    uint64_t avg_write_done_us = 0;
};
struct RoundReceiveSummary {
    uint64_t direct_grant_ops = 0;
    uint64_t staged_grant_ops = 0;
    uint64_t staged_delivery_ops = 0;
    uint64_t staged_copy_bytes = 0;
    uint64_t staged_copy_us = 0;
};
std::string render_round_summary(const char* role,
                                 size_t round,
                                 size_t size,
                                 int iters,
                                 size_t total_bytes,
                                 uint64_t elapsed_us,
                                 const RoundTimingSummary* timing = nullptr,
                                 const RoundReceiveSummary* receive = nullptr);
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
        "client", 2, 4u * 1024u * 1024u, 10, 40u * 1024u * 1024u, 12345, nullptr, nullptr);
    EXPECT_NE(line.find("ALPS_KV_ROUND"), std::string::npos);
    EXPECT_NE(line.find("role=client"), std::string::npos);
    EXPECT_NE(line.find("round=2"), std::string::npos);
    EXPECT_NE(line.find("size=4194304"), std::string::npos);
    EXPECT_NE(line.find("iters=10"), std::string::npos);
}

TEST(AlpsKvBenchHelpersTest, RoundSummaryIncludesWriteTimingWhenProvided) {
    const zerokv::examples::alps_kv_bench::RoundTimingSummary timing{
        .avg_control_request_grant_us = 123,
        .avg_put_us = 456,
        .avg_flush_us = 789,
        .avg_write_done_us = 42,
    };

    const auto line = zerokv::examples::alps_kv_bench::render_round_summary(
        "client", 1, 32u * 1024u * 1024u, 4, 128u * 1024u * 1024u, 77777, &timing, nullptr);
    EXPECT_NE(line.find("avg_control_request_grant_us=123"), std::string::npos);
    EXPECT_NE(line.find("avg_put_us=456"), std::string::npos);
    EXPECT_NE(line.find("avg_flush_us=789"), std::string::npos);
    EXPECT_NE(line.find("avg_write_done_us=42"), std::string::npos);
}

TEST(AlpsKvBenchHelpersTest, RoundSummaryIncludesReceiveStatsWhenProvided) {
    const zerokv::examples::alps_kv_bench::RoundReceiveSummary receive{
        .direct_grant_ops = 8,
        .staged_grant_ops = 2,
        .staged_delivery_ops = 2,
        .staged_copy_bytes = 4096,
        .staged_copy_us = 55,
    };

    const auto line = zerokv::examples::alps_kv_bench::render_round_summary(
        "server", 1, 32u * 1024u * 1024u, 4, 128u * 1024u * 1024u, 77777, nullptr, &receive);
    EXPECT_NE(line.find("direct_grant_ops=8"), std::string::npos);
    EXPECT_NE(line.find("staged_grant_ops=2"), std::string::npos);
    EXPECT_NE(line.find("staged_delivery_ops=2"), std::string::npos);
    EXPECT_NE(line.find("staged_copy_bytes=4096"), std::string::npos);
    EXPECT_NE(line.find("staged_copy_us=55"), std::string::npos);
}

TEST(AlpsKvBenchHelpersTest, ListenAddressLineIncludesResolvedAddress) {
    const auto line = zerokv::examples::alps_kv_bench::render_listen_address_line("10.0.2.15:16000");
    EXPECT_NE(line.find("ALPS_KV_LISTEN"), std::string::npos);
    EXPECT_NE(line.find("address=10.0.2.15:16000"), std::string::npos);
}
