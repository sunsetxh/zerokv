#include <gtest/gtest.h>

#include "p2p/cluster.h"
#include "p2p/config.h"
#include "p2p/worker.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <array>
#include <thread>

using namespace p2p;

namespace {

constexpr uint32_t kControlMagic = 0x50325031U;
constexpr uint16_t kControlHeaderVersion = 1;

struct RawControlFrameHeader {
    uint32_t magic = kControlMagic;
    uint16_t header_version = kControlHeaderVersion;
    uint16_t type = 1;
    uint32_t payload_size = 0;
    uint32_t reserved = 0;
};

uint16_t reserve_test_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return 0;
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return 0;
    }

    const auto port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

bool wait_until(const std::function<bool()>& pred,
                std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return pred();
}

int connect_raw_socket(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

class ClusterDiscoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_ = Config::builder()
            .set_transport("tcp")
            .build();
        context_ = Context::create(config_);
        if (!context_) {
            GTEST_SKIP() << "Context creation failed";
        }
    }

    Config config_;
    Context::Ptr context_;
};

TEST_F(ClusterDiscoveryTest, MasterAndSlaveExchangeMembership) {
    const auto port = reserve_test_port();
    ASSERT_NE(port, 0);

    auto master_worker = Worker::create(context_, 0);
    auto slave_worker = Worker::create(context_, 1);
    ASSERT_NE(master_worker, nullptr);
    ASSERT_NE(slave_worker, nullptr);

    auto master = Cluster::create_master(context_, master_worker, MasterClusterConfig{
        .control_bind_addr = "127.0.0.1:" + std::to_string(port),
    });
    auto slave = Cluster::create_slave(context_, slave_worker, SlaveClusterConfig{
        .rank = 1,
        .master_control_addr = "127.0.0.1:" + std::to_string(port),
    });
    ASSERT_NE(master, nullptr);
    ASSERT_NE(slave, nullptr);

    ASSERT_TRUE(master->start().ok());
    ASSERT_TRUE(slave->start().ok());

    EXPECT_TRUE(wait_until([&]() {
        return master->membership() && master->membership()->peers.size() == 2;
    }));
    EXPECT_TRUE(wait_until([&]() {
        return slave->membership() && slave->membership()->peers.size() == 2;
    }));

    ASSERT_NE(master->membership(), nullptr);
    ASSERT_NE(slave->membership(), nullptr);
    EXPECT_EQ(master->membership()->epoch, 2u);
    EXPECT_EQ(slave->membership()->epoch, 2u);
    EXPECT_EQ(master->resolve_rank("RANK1"), 1u);
    EXPECT_EQ(slave->resolve_rank("RANK0"), 0u);
    EXPECT_TRUE(master->wait_ready().status().ok());
    EXPECT_TRUE(slave->wait_ready().status().ok());

    EXPECT_TRUE(slave->stop().ok());
    EXPECT_TRUE(wait_until([&]() {
        return master->membership() && master->membership()->peers.size() == 1;
    }));
    ASSERT_NE(master->membership(), nullptr);
    EXPECT_EQ(master->membership()->epoch, 3u);
    EXPECT_EQ(master->resolve_rank("RANK1"), std::nullopt);

    EXPECT_TRUE(master->stop().ok());
}

TEST_F(ClusterDiscoveryTest, DuplicateSlaveRankIsRejectedWithoutCorruptingMembership) {
    const auto port = reserve_test_port();
    ASSERT_NE(port, 0);

    auto master_worker = Worker::create(context_, 0);
    auto slave1_worker = Worker::create(context_, 1);
    auto slave2_worker = Worker::create(context_, 2);
    ASSERT_NE(master_worker, nullptr);
    ASSERT_NE(slave1_worker, nullptr);
    ASSERT_NE(slave2_worker, nullptr);

    auto master = Cluster::create_master(context_, master_worker, MasterClusterConfig{
        .control_bind_addr = "127.0.0.1:" + std::to_string(port),
    });
    auto slave1 = Cluster::create_slave(context_, slave1_worker, SlaveClusterConfig{
        .rank = 1,
        .master_control_addr = "127.0.0.1:" + std::to_string(port),
    });
    auto slave2 = Cluster::create_slave(context_, slave2_worker, SlaveClusterConfig{
        .rank = 1,
        .master_control_addr = "127.0.0.1:" + std::to_string(port),
    });
    ASSERT_NE(master, nullptr);
    ASSERT_NE(slave1, nullptr);
    ASSERT_NE(slave2, nullptr);

    ASSERT_TRUE(master->start().ok());
    ASSERT_TRUE(slave1->start().ok());

    const auto duplicate_status = slave2->start();
    EXPECT_FALSE(duplicate_status.ok());
    EXPECT_EQ(duplicate_status.code(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(slave2->state(), ClusterState::kFailed);

    EXPECT_TRUE(wait_until([&]() {
        return master->membership() && master->membership()->peers.size() == 2;
    }));
    ASSERT_NE(master->membership(), nullptr);
    EXPECT_EQ(master->membership()->epoch, 2u);
    EXPECT_EQ(master->resolve_rank("RANK1"), 1u);

    EXPECT_TRUE(slave1->stop().ok());
    EXPECT_TRUE(master->stop().ok());
}

TEST_F(ClusterDiscoveryTest, ConnectedRoutesCanSendByAlias) {
    const auto port = reserve_test_port();
    ASSERT_NE(port, 0);

    auto master_worker = Worker::create(context_, 0);
    auto slave_worker = Worker::create(context_, 1);
    ASSERT_NE(master_worker, nullptr);
    ASSERT_NE(slave_worker, nullptr);

    auto master = Cluster::create_master(context_, master_worker, MasterClusterConfig{
        .control_bind_addr = "127.0.0.1:" + std::to_string(port),
    });
    auto slave = Cluster::create_slave(context_, slave_worker, SlaveClusterConfig{
        .rank = 1,
        .master_control_addr = "127.0.0.1:" + std::to_string(port),
    });
    ASSERT_NE(master, nullptr);
    ASSERT_NE(slave, nullptr);

    ASSERT_TRUE(master->start().ok());
    ASSERT_TRUE(slave->start().ok());

    EXPECT_TRUE(wait_until([&]() {
        const auto master_route = master->route("RANK1");
        const auto slave_route = slave->route("RANK0");
        return master_route.has_value() && master_route->connected &&
               slave_route.has_value() && slave_route->connected;
    }));

    std::array<char, 32> buffer{};
    constexpr Tag kTag = 0xCAFE;
    constexpr char kPayload[] = "hello-rank0";

    auto recv_future = master->recv_any(buffer.data(), buffer.size(), kTag);
    auto send_future = slave->send("RANK0", kPayload, sizeof(kPayload), kTag);

    EXPECT_TRUE(wait_until([&]() {
        return send_future.ready() && recv_future.ready();
    }));
    EXPECT_TRUE(send_future.status().ok()) << send_future.status().message();
    auto recv_result = recv_future.get(std::chrono::milliseconds{2000});
    ASSERT_TRUE(recv_result.has_value());
    EXPECT_STREQ(buffer.data(), kPayload);

    EXPECT_TRUE(slave->stop().ok());
    EXPECT_TRUE(master->stop().ok());
}

TEST_F(ClusterDiscoveryTest, SlavesCanSendToEachOtherByAlias) {
    const auto port = reserve_test_port();
    ASSERT_NE(port, 0);

    auto master_worker = Worker::create(context_, 0);
    auto slave1_worker = Worker::create(context_, 1);
    auto slave2_worker = Worker::create(context_, 2);
    ASSERT_NE(master_worker, nullptr);
    ASSERT_NE(slave1_worker, nullptr);
    ASSERT_NE(slave2_worker, nullptr);

    auto master = Cluster::create_master(context_, master_worker, MasterClusterConfig{
        .control_bind_addr = "127.0.0.1:" + std::to_string(port),
    });
    auto slave1 = Cluster::create_slave(context_, slave1_worker, SlaveClusterConfig{
        .rank = 1,
        .master_control_addr = "127.0.0.1:" + std::to_string(port),
    });
    auto slave2 = Cluster::create_slave(context_, slave2_worker, SlaveClusterConfig{
        .rank = 2,
        .master_control_addr = "127.0.0.1:" + std::to_string(port),
    });
    ASSERT_NE(master, nullptr);
    ASSERT_NE(slave1, nullptr);
    ASSERT_NE(slave2, nullptr);

    ASSERT_TRUE(master->start().ok());
    ASSERT_TRUE(slave1->start().ok());
    ASSERT_TRUE(slave2->start().ok());

    EXPECT_TRUE(wait_until([&]() {
        const auto route12 = slave1->route("RANK2");
        const auto route21 = slave2->route("RANK1");
        return route12.has_value() && route12->connected &&
               route21.has_value() && route21->connected;
    }));

    std::array<char, 32> buffer{};
    constexpr Tag kTag = 0xBEEF;
    constexpr char kPayload[] = "hello-rank2";

    auto recv_future = slave2->recv_any(buffer.data(), buffer.size(), kTag);
    auto send_future = slave1->send("RANK2", kPayload, sizeof(kPayload), kTag);

    EXPECT_TRUE(wait_until([&]() {
        return send_future.ready() && recv_future.ready();
    }));
    EXPECT_TRUE(send_future.status().ok()) << send_future.status().message();
    auto recv_result = recv_future.get(std::chrono::milliseconds{2000});
    ASSERT_TRUE(recv_result.has_value());
    EXPECT_STREQ(buffer.data(), kPayload);

    EXPECT_TRUE(slave2->stop().ok());
    EXPECT_TRUE(slave1->stop().ok());
    EXPECT_TRUE(master->stop().ok());
}

TEST_F(ClusterDiscoveryTest, OversizedControlFrameIsRejected) {
    const auto port = reserve_test_port();
    ASSERT_NE(port, 0);

    auto master_worker = Worker::create(context_, 0);
    ASSERT_NE(master_worker, nullptr);

    auto master = Cluster::create_master(context_, master_worker, MasterClusterConfig{
        .control_bind_addr = "127.0.0.1:" + std::to_string(port),
    });
    ASSERT_NE(master, nullptr);
    ASSERT_TRUE(master->start().ok());

    const int raw_fd = connect_raw_socket(port);
    ASSERT_GE(raw_fd, 0);

    RawControlFrameHeader header;
    header.payload_size = (1u << 20) + 1;
    ASSERT_EQ(::send(raw_fd, &header, sizeof(header), MSG_NOSIGNAL),
              static_cast<ssize_t>(sizeof(header)));

    EXPECT_TRUE(wait_until([&]() {
        char byte = 0;
        const auto rc = ::recv(raw_fd, &byte, 1, MSG_DONTWAIT);
        return rc == 0 || (rc < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
    }, std::chrono::milliseconds{500}));

    ::close(raw_fd);

    auto slave_worker = Worker::create(context_, 1);
    ASSERT_NE(slave_worker, nullptr);
    auto slave = Cluster::create_slave(context_, slave_worker, SlaveClusterConfig{
        .rank = 1,
        .master_control_addr = "127.0.0.1:" + std::to_string(port),
    });
    ASSERT_NE(slave, nullptr);
    EXPECT_TRUE(slave->start().ok());

    EXPECT_TRUE(slave->stop().ok());
    EXPECT_TRUE(master->stop().ok());
}
