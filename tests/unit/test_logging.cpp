#include <gtest/gtest.h>

#include "internal/logging.h"

namespace zerokv::detail {
namespace {

class LoggingTest : public ::testing::Test {
protected:
    void SetUp() override {
        unsetenv("ZEROKV_LOG_LEVEL");
        reset_log_level_for_tests();
    }

    void TearDown() override {
        unsetenv("ZEROKV_LOG_LEVEL");
        reset_log_level_for_tests();
    }
};

TEST_F(LoggingTest, DefaultLevelIsError) {
    EXPECT_EQ(current_log_level(), LogLevel::kError);
}

TEST_F(LoggingTest, ParseKnownLevels) {
    EXPECT_EQ(parse_log_level("error"), LogLevel::kError);
    EXPECT_EQ(parse_log_level("warn"), LogLevel::kWarn);
    EXPECT_EQ(parse_log_level("info"), LogLevel::kInfo);
    EXPECT_EQ(parse_log_level("debug"), LogLevel::kDebug);
    EXPECT_EQ(parse_log_level("trace"), LogLevel::kTrace);
    EXPECT_EQ(parse_log_level("WARNING"), LogLevel::kWarn);
}

TEST_F(LoggingTest, UnknownLevelFallsBackToError) {
    EXPECT_EQ(parse_log_level("nonsense"), LogLevel::kError);
}

TEST_F(LoggingTest, ReadsLevelFromEnvironment) {
    setenv("ZEROKV_LOG_LEVEL", "debug", 1);
    reset_log_level_for_tests();
    EXPECT_EQ(current_log_level(), LogLevel::kDebug);
}

TEST_F(LoggingTest, FiltersByLevel) {
    setenv("ZEROKV_LOG_LEVEL", "warn", 1);
    reset_log_level_for_tests();

    EXPECT_TRUE(log_enabled(LogLevel::kError));
    EXPECT_TRUE(log_enabled(LogLevel::kWarn));
    EXPECT_FALSE(log_enabled(LogLevel::kInfo));
    EXPECT_FALSE(log_enabled(LogLevel::kDebug));
    EXPECT_FALSE(log_enabled(LogLevel::kTrace));
}

TEST_F(LoggingTest, TraceEnablesAllLevels) {
    setenv("ZEROKV_LOG_LEVEL", "trace", 1);
    reset_log_level_for_tests();

    EXPECT_TRUE(log_enabled(LogLevel::kError));
    EXPECT_TRUE(log_enabled(LogLevel::kWarn));
    EXPECT_TRUE(log_enabled(LogLevel::kInfo));
    EXPECT_TRUE(log_enabled(LogLevel::kDebug));
    EXPECT_TRUE(log_enabled(LogLevel::kTrace));
}

}  // namespace
}  // namespace zerokv::detail
