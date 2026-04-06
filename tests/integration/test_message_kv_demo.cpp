#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zerokv::examples::message_kv_demo {
enum class SendMode {
    kSync,
    kAsync,
};
std::vector<size_t> parse_sizes_csv(const std::string& csv);
SendMode parse_send_mode(const std::string& value);
const char* send_mode_name(SendMode mode);
std::string make_round_key(size_t round_index, size_t size_bytes, size_t thread_index);
std::string make_payload(size_t round_index, size_t size_bytes, size_t thread_index);
size_t max_size_bytes_for_sizes(const std::vector<size_t>& sizes);
size_t max_total_recv_bytes_for_sizes(size_t messages, const std::vector<size_t>& sizes);
bool should_sleep_after_round(size_t round_index, size_t total_rounds);
std::string render_send_round_summary(size_t round_index,
                                      const char* send_mode,
                                      size_t size_bytes,
                                      size_t messages,
                                      uint64_t send_total_us,
                                      uint64_t max_thread_send_us,
                                      size_t total_bytes);
std::string render_recv_round_summary(size_t round_index,
                                      size_t size_bytes,
                                      size_t completed,
                                      size_t failed,
                                      size_t timed_out,
                                      bool completed_all,
                                      uint64_t recv_total_us,
                                      size_t total_bytes);
std::string make_compact_preview(const void* data, size_t size_bytes, size_t max_bytes);
}  // namespace zerokv::examples::message_kv_demo

TEST(MessageKvDemoHelpersTest, ParsesDefaultLikeSizeCsv) {
    auto sizes = zerokv::examples::message_kv_demo::parse_sizes_csv("1K,64K,1M");
    ASSERT_EQ(sizes.size(), 3u);
    EXPECT_EQ(sizes[0], 1024u);
    EXPECT_EQ(sizes[1], 64u * 1024u);
    EXPECT_EQ(sizes[2], 1024u * 1024u);
}

TEST(MessageKvDemoHelpersTest, ParseSendModeAcceptsSyncAndAsync) {
    EXPECT_EQ(zerokv::examples::message_kv_demo::send_mode_name(
                  zerokv::examples::message_kv_demo::parse_send_mode("sync")),
              std::string("sync"));
    EXPECT_EQ(zerokv::examples::message_kv_demo::send_mode_name(
                  zerokv::examples::message_kv_demo::parse_send_mode("async")),
              std::string("async"));
    EXPECT_THROW(zerokv::examples::message_kv_demo::parse_send_mode("bad"),
                 std::invalid_argument);
}

TEST(MessageKvDemoHelpersTest, RoundKeyUsesRawByteSize) {
    EXPECT_EQ(zerokv::examples::message_kv_demo::make_round_key(2, 1048576, 3),
              "msg-round2-size1048576-thread3");
}

TEST(MessageKvDemoHelpersTest, PayloadHasExactRequestedLength) {
    auto payload = zerokv::examples::message_kv_demo::make_payload(1, 1024, 2);
    EXPECT_EQ(payload.size(), 1024u);
    EXPECT_TRUE(payload.rfind("round1-thread2-", 0) == 0);
}

TEST(MessageKvDemoHelpersTest, MaxSizeBytesUsesLargestConfiguredPayload) {
    const std::vector<size_t> sizes = {1024u, 64u * 1024u, 1024u * 1024u};
    EXPECT_EQ(zerokv::examples::message_kv_demo::max_size_bytes_for_sizes(sizes),
              1024u * 1024u);
}

TEST(MessageKvDemoHelpersTest, MaxTotalRecvBytesUsesLargestConfiguredPayloadAndMessageCount) {
    const std::vector<size_t> sizes = {1024u, 64u * 1024u, 1024u * 1024u};
    EXPECT_EQ(
        zerokv::examples::message_kv_demo::max_total_recv_bytes_for_sizes(4, sizes),
        4u * 1024u * 1024u);
}

TEST(MessageKvDemoHelpersTest, PostRecvWaitOnlyRunsAfterFinalMeasuredRound) {
    EXPECT_FALSE(zerokv::examples::message_kv_demo::should_sleep_after_round(0, 3));
    EXPECT_FALSE(zerokv::examples::message_kv_demo::should_sleep_after_round(1, 3));
    EXPECT_TRUE(zerokv::examples::message_kv_demo::should_sleep_after_round(2, 3));
}

TEST(MessageKvDemoHelpersTest, SendRoundSummaryUsesConfiguredSizeAndCounts) {
    auto line = zerokv::examples::message_kv_demo::render_send_round_summary(
        3,
        "async",
        65536,
        4,
        1200,
        500,
        262144);
    EXPECT_NE(line.find("SEND_ROUND"), std::string::npos);
    EXPECT_NE(line.find("round=3"), std::string::npos);
    EXPECT_NE(line.find("send_mode=async"), std::string::npos);
    EXPECT_NE(line.find("size=65536"), std::string::npos);
    EXPECT_NE(line.find("messages=4"), std::string::npos);
    EXPECT_NE(line.find("total_bytes=262144"), std::string::npos);
}

TEST(MessageKvDemoHelpersTest, RecvRoundSummaryUsesCompletedCountsAndThroughput) {
    auto line = zerokv::examples::message_kv_demo::render_recv_round_summary(
        2, 4096, 4, 0, 0, true, 1500, 16384);
    EXPECT_NE(line.find("RECV_ROUND"), std::string::npos);
    EXPECT_NE(line.find("round=2"), std::string::npos);
    EXPECT_NE(line.find("size=4096"), std::string::npos);
    EXPECT_NE(line.find("completed=4"), std::string::npos);
    EXPECT_NE(line.find("failed=0"), std::string::npos);
    EXPECT_NE(line.find("timed_out=0"), std::string::npos);
    EXPECT_NE(line.find("completed_all=1"), std::string::npos);
    EXPECT_NE(line.find("recv_total_us=1500"), std::string::npos);
    EXPECT_NE(line.find("total_bytes=16384"), std::string::npos);
}

TEST(MessageKvDemoHelpersTest, CompactPreviewTruncatesLongPayloads) {
    const std::string payload = "round0-thread1-abcdefghijklmnopqrstuvwxyz";
    auto preview = zerokv::examples::message_kv_demo::make_compact_preview(
        payload.data(), payload.size(), 16);
    EXPECT_EQ(preview, "round0-thread1-a...");
}
