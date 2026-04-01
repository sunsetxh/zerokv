#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace zerokv::examples::message_kv_demo {
std::vector<size_t> parse_sizes_csv(const std::string& csv);
std::string make_round_key(size_t round_index, size_t size_bytes, size_t thread_index);
std::string make_payload(size_t round_index, size_t size_bytes, size_t thread_index);
}  // namespace zerokv::examples::message_kv_demo

TEST(MessageKvDemoHelpersTest, ParsesDefaultLikeSizeCsv) {
    auto sizes = zerokv::examples::message_kv_demo::parse_sizes_csv("1K,64K,1M");
    ASSERT_EQ(sizes.size(), 3u);
    EXPECT_EQ(sizes[0], 1024u);
    EXPECT_EQ(sizes[1], 64u * 1024u);
    EXPECT_EQ(sizes[2], 1024u * 1024u);
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
