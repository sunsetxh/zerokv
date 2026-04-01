#include "internal/listener_addr.h"

#include <gtest/gtest.h>

using namespace zerokv::internal;

TEST(ListenerAddrTest, KeepsExplicitHostAndUsesQueriedPort) {
    EXPECT_EQ(select_listener_address("10.0.0.2:0", "10.0.2.15:60535"),
              "10.0.0.2:60535");
}

TEST(ListenerAddrTest, UsesQueriedAddressForWildcardBind) {
    EXPECT_EQ(select_listener_address("0.0.0.0:0", "10.0.2.15:60535"),
              "10.0.2.15:60535");
}

TEST(ListenerAddrTest, FallsBackToQueriedWhenPortMissing) {
    EXPECT_EQ(select_listener_address("10.0.0.2:0", "10.0.2.15"),
              "10.0.2.15");
}
