#include "zerokv/message_kv.h"

#include <gtest/gtest.h>

TEST(MessageKvApiSurfaceTest, PublicTypesExist) {
    using zerokv::MessageKV;

    MessageKV::BatchRecvItem item;
    item.key = "k";
    item.length = 16;
    item.offset = 0;

    MessageKV::BatchRecvResult result;
    EXPECT_TRUE(result.completed.empty());
    EXPECT_FALSE(result.completed_all);
}
