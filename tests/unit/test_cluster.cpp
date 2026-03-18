#include <gtest/gtest.h>

#include "axon/cluster.h"
#include "axon/config.h"
#include "axon/worker.h"

using namespace axon;

TEST(ClusterTypesTest, RankAliasRoundTrip) {
    EXPECT_EQ(rank_alias(0), "RANK0");
    EXPECT_EQ(parse_rank_alias("RANK0"), 0u);
    EXPECT_EQ(parse_rank_alias("RANK123"), 123u);
}

TEST(ClusterTypesTest, RejectInvalidAlias) {
    EXPECT_EQ(parse_rank_alias(""), std::nullopt);
    EXPECT_EQ(parse_rank_alias("rank1"), std::nullopt);
    EXPECT_EQ(parse_rank_alias("RANK"), std::nullopt);
    EXPECT_EQ(parse_rank_alias("RANKX"), std::nullopt);
    EXPECT_EQ(parse_rank_alias("NODE1"), std::nullopt);
}

class ClusterTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = Context::create(Config::builder().set_transport("tcp").build());
        if (!context_) {
            GTEST_SKIP() << "Context creation failed";
        }
        worker_ = Worker::create(context_);
        if (!worker_) {
            GTEST_SKIP() << "Worker creation failed";
        }
    }

    Context::Ptr context_;
    Worker::Ptr worker_;
};

TEST_F(ClusterTest, CreateMasterInitializesRankZeroMembership) {
    auto cluster = Cluster::create_master(context_, worker_, MasterClusterConfig{
        .control_bind_addr = "127.0.0.1:7000",
    });
    ASSERT_NE(cluster, nullptr);
    EXPECT_TRUE(cluster->is_master());
    EXPECT_EQ(cluster->self_rank(), 0u);
    EXPECT_EQ(cluster->self_alias(), "RANK0");
    ASSERT_NE(cluster->membership(), nullptr);
    EXPECT_EQ(cluster->membership()->epoch, 1u);
    ASSERT_EQ(cluster->membership()->peers.size(), 1u);
    EXPECT_EQ(cluster->membership()->peers[0].rank, 0u);
    EXPECT_TRUE(cluster->membership()->peers[0].is_master);
}

TEST_F(ClusterTest, CreateSlaveRejectsReservedRankZero) {
    auto cluster = Cluster::create_slave(context_, worker_, SlaveClusterConfig{
        .rank = 0,
        .master_control_addr = "127.0.0.1:7000",
    });
    EXPECT_EQ(cluster, nullptr);
}

TEST_F(ClusterTest, CreateSlaveInitializesAliasAndRouteLookup) {
    auto cluster = Cluster::create_slave(context_, worker_, SlaveClusterConfig{
        .rank = 3,
        .master_control_addr = "127.0.0.1:7000",
    });
    ASSERT_NE(cluster, nullptr);
    EXPECT_FALSE(cluster->is_master());
    EXPECT_EQ(cluster->self_rank(), 3u);
    EXPECT_EQ(cluster->self_alias(), "RANK3");
    EXPECT_EQ(cluster->resolve_rank("RANK3"), 3u);
    ASSERT_NE(cluster->membership(), nullptr);
    EXPECT_EQ(cluster->membership()->epoch, 0u);

    auto route = cluster->route("RANK2");
    EXPECT_FALSE(route.has_value());
}

TEST_F(ClusterTest, StartMasterTransitionsToReady) {
    auto cluster = Cluster::create_master(context_, worker_, MasterClusterConfig{
        .control_bind_addr = "127.0.0.1:0",
    });
    ASSERT_NE(cluster, nullptr);
    EXPECT_EQ(cluster->state(), ClusterState::kInit);

    auto status = cluster->start();
    EXPECT_TRUE(status.ok()) << status.message();
    EXPECT_EQ(cluster->state(), ClusterState::kReady);
}

TEST_F(ClusterTest, WaitReadyReportsErrorWhileClusterIsUnstarted) {
    auto cluster = Cluster::create_master(context_, worker_, MasterClusterConfig{
        .control_bind_addr = "127.0.0.1:7000",
    });
    ASSERT_NE(cluster, nullptr);

    auto ready = cluster->wait_ready();
    EXPECT_TRUE(ready.ready());
    EXPECT_EQ(ready.status().code(), ErrorCode::kNotImplemented);
}

TEST_F(ClusterTest, StartMakesWaitReadySucceed) {
    auto cluster = Cluster::create_master(context_, worker_, MasterClusterConfig{
        .control_bind_addr = "127.0.0.1:0",
    });
    ASSERT_NE(cluster, nullptr);

    auto status = cluster->start();
    EXPECT_TRUE(status.ok()) << status.message();

    auto ready = cluster->wait_ready();
    EXPECT_TRUE(ready.ready());
    EXPECT_TRUE(ready.status().ok()) << ready.status().message();

    EXPECT_TRUE(cluster->stop().ok());
}

TEST_F(ClusterTest, WaitReadyTimeoutReturnsTimeout) {
    auto cluster = Cluster::create_master(context_, worker_, MasterClusterConfig{
        .control_bind_addr = "127.0.0.1:0",
        .min_cluster_size = 2,
    });
    ASSERT_NE(cluster, nullptr);
    ASSERT_TRUE(cluster->start().ok());

    auto ready = cluster->wait_ready(std::chrono::milliseconds{20});
    EXPECT_TRUE(ready.ready());
    EXPECT_EQ(ready.status().code(), ErrorCode::kTimeout);

    EXPECT_TRUE(cluster->stop().ok());
}

TEST_F(ClusterTest, RouteOnlyExistsForCurrentMembership) {
    auto cluster = Cluster::create_master(context_, worker_, MasterClusterConfig{
        .control_bind_addr = "127.0.0.1:7000",
    });
    ASSERT_NE(cluster, nullptr);

    auto self_route = cluster->route("RANK0");
    ASSERT_TRUE(self_route.has_value());
    EXPECT_EQ(self_route->rank, 0u);
    EXPECT_EQ(self_route->alias, "RANK0");
    EXPECT_TRUE(self_route->connected);

    EXPECT_EQ(cluster->resolve_rank("RANK1"), std::nullopt);
    EXPECT_EQ(cluster->route("RANK1"), std::nullopt);
}
