#include <gtest/gtest.h>
#include "axon/config.h"

using namespace axon;

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
    setenv("AXON_TRANSPORT", "tcp", 1);
    auto config = Config::builder()
        .from_env()
        .build();
    EXPECT_EQ(config.transport(), "tcp");
}

TEST(ConfigTest, InvalidUcXOptionFailsContextCreation) {
    auto config = Config::builder()
        .set("UCX_TLS", "__definitely_invalid_transport__")
        .build();

    auto context = Context::create(config);
    EXPECT_EQ(context, nullptr);
}
