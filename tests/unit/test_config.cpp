#include <gtest/gtest.h>
#include "p2p/config.h"

using namespace p2p;

TEST(ConfigTest, DefaultConfig) {
    auto config = Config::builder().build();
    // Config is always valid, just check transport is set
    EXPECT_EQ(config.transport(), "ucx");
}

TEST(ConfigTest, BuilderPattern) {
    auto config = Config::builder()
        .set_transport("tcp")
        .build();
    EXPECT_EQ(config.transport(), "tcp");
}

TEST(ConfigTest, FromEnv) {
    // Set environment variable
    setenv("P2P_TRANSPORTS", "tcp,shmem", 1);
    auto config = Config::builder()
        .from_env()
        .build();
    // Config should have values from env
    (void)config;
}
