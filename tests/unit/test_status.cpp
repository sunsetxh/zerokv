#include <gtest/gtest.h>
#include "axon/common.h"

using namespace axon;

TEST(StatusTest, DefaultStatus) {
    Status s;
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(s.code(), ErrorCode::kSuccess);
}

TEST(StatusTest, ErrorStatus) {
    Status s(ErrorCode::kInvalidArgument);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), ErrorCode::kInvalidArgument);
}

TEST(StatusTest, ErrorStatusWithMessage) {
    Status s(ErrorCode::kConnectionRefused, "Connection refused");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), ErrorCode::kConnectionRefused);
    EXPECT_EQ(s.message(), "Connection refused");
}

TEST(StatusTest, InProgressStatus) {
    Status s(ErrorCode::kInProgress);
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(s.in_progress());
}

TEST(StatusTest, ErrorCodeConversion) {
    std::error_code ec = make_error_code(ErrorCode::kOutOfMemory);
    EXPECT_EQ(ec.value(), static_cast<int>(ErrorCode::kOutOfMemory));
}

TEST(StatusTest, ThrowIfError) {
    Status ok_status;
    EXPECT_NO_THROW(ok_status.throw_if_error());

    Status error_status(ErrorCode::kTransportError);
    EXPECT_THROW(error_status.throw_if_error(), std::system_error);
}
