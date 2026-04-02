#include <gtest/gtest.h>
#include "zerokv/config.h"
#include "zerokv/transport/worker.h"
#include "zerokv/transport/endpoint.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <thread>
#include <chrono>
#include <vector>
#include <cstring>
#include <atomic>

using namespace zerokv;
using namespace zerokv::transport;

namespace {

std::string find_non_loopback_ipv4() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0 || ifaddr == nullptr) {
        return {};
    }

    std::string result;
    for (auto* it = ifaddr; it != nullptr; it = it->ifa_next) {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        const auto* addr = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
        if (ntohl(addr->sin_addr.s_addr) == INADDR_LOOPBACK) {
            continue;
        }

        char buf[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf)) != nullptr) {
            result = buf;
            break;
        }
    }

    freeifaddrs(ifaddr);
    return result;
}

}  // namespace

// =============================================================================
// Loopback Communication Tests (single process, dual worker)
// =============================================================================

class LoopbackTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_ = Config::builder()
            .set_transport("tcp")
            .build();
        
        context_ = Context::create(config_);
        if (!context_) {
            GTEST_SKIP() << "UCX context creation failed";
        }
    }
    
    Config config_;
    Context::Ptr context_;
};

// Test basic worker creation
TEST_F(LoopbackTest, WorkerCreation) {
    auto worker = Worker::create(context_);
    ASSERT_NE(worker, nullptr);
}

// Test worker address retrieval
TEST_F(LoopbackTest, WorkerAddress) {
    auto worker = Worker::create(context_);
    auto addr = worker->address();
    EXPECT_GT(addr.size(), 0u) << "Worker address should not be empty";
}

// Test listener creation
TEST_F(LoopbackTest, ListenerCreation) {
    auto worker = Worker::create(context_);
    
    std::atomic<bool> accepted{false};
    auto listener = worker->listen("0.0.0.0:0", [&](Endpoint::Ptr ep) {
        (void)ep; accepted = true;
    });
    
    ASSERT_NE(listener, nullptr);
    EXPECT_FALSE(listener->address().empty());
    
    listener->close();
}

TEST_F(LoopbackTest, ListenerPreservesExplicitBindHostForEphemeralPort) {
    const auto bind_ip = find_non_loopback_ipv4();
    if (bind_ip.empty()) {
        GTEST_SKIP() << "No non-loopback IPv4 address available";
    }

    auto worker = Worker::create(context_);
    ASSERT_NE(worker, nullptr);

    auto listener = worker->listen(bind_ip + ":0", [&](Endpoint::Ptr ep) {
        (void)ep;
    });

    ASSERT_NE(listener, nullptr);
    EXPECT_EQ(listener->address().rfind(bind_ip + ":", 0), 0u);
}

// Test that connect(address_blob) works
TEST_F(LoopbackTest, ConnectAddressBlob) {
    auto worker1 = Worker::create(context_);
    auto worker2 = Worker::create(context_);
    
    // Get worker2's address
    auto addr2 = worker2->address();
    EXPECT_GT(addr2.size(), 0u);
    
    // Connect worker1 to worker2 using address blob
    auto connect_future = worker1->connect(addr2);
    
    // Progress to complete connection
    for (int i = 0; i < 100 && !connect_future.ready(); ++i) {
        worker1->progress();
        worker2->progress();
    }
    
    // Should succeed
    EXPECT_TRUE(connect_future.ready());
    EXPECT_TRUE(connect_future.status().ok()) << "Status: " << connect_future.status().message();
    
    auto ep = connect_future.get();
    EXPECT_NE(ep, nullptr);
}

// Test worker-level tag_recv can be posted
TEST_F(LoopbackTest, TagRecvCanBePosted) {
    auto worker = Worker::create(context_);

    std::vector<char> buffer(1024, 0);
    auto future = worker->tag_recv(buffer.data(), buffer.size(), 100);

    // Should return a valid future (even though no data will arrive)
    EXPECT_NE(future.request(), nullptr);
}

TEST_F(LoopbackTest, EndpointCloseReturnsTrackedRequest) {
    auto worker1 = Worker::create(context_);
    auto worker2 = Worker::create(context_);
    ASSERT_NE(worker1, nullptr);
    ASSERT_NE(worker2, nullptr);

    auto connect_future = worker1->connect(worker2->address());
    ASSERT_TRUE(connect_future.status().ok());
    auto ep = connect_future.get();
    ASSERT_NE(ep, nullptr);

    auto close_future = ep->close();
    EXPECT_NE(close_future.request(), nullptr);
    EXPECT_FALSE(ep->is_connected());

    for (int i = 0; i < 200 && !close_future.ready(); ++i) {
        worker1->progress();
        worker2->progress();
    }
    EXPECT_TRUE(close_future.ready());
}

TEST_F(LoopbackTest, EndpointTagRecvReceivesData) {
    auto worker1 = Worker::create(context_);
    auto worker2 = Worker::create(context_);
    ASSERT_NE(worker1, nullptr);
    ASSERT_NE(worker2, nullptr);

    auto sender = worker1->connect(worker2->address()).get();
    auto receiver = worker2->connect(worker1->address()).get();
    ASSERT_NE(sender, nullptr);
    ASSERT_NE(receiver, nullptr);

    std::vector<char> buffer(64, 0);

    constexpr Tag kTag = 77;
    auto recv_future = receiver->tag_recv(buffer.data(), buffer.size(), kTag);

    const char payload[] = "loopback-region";
    auto send_future = sender->tag_send(payload, sizeof(payload), kTag);

    for (int i = 0; i < 500 && (!recv_future.ready() || !send_future.ready()); ++i) {
        worker1->progress();
        worker2->progress();
    }

    ASSERT_TRUE(send_future.ready());
    ASSERT_TRUE(recv_future.ready());

    auto [bytes, matched_tag] = recv_future.get();
    EXPECT_EQ(matched_tag, kTag);
    EXPECT_EQ(bytes, sizeof(payload));
    EXPECT_EQ(std::memcmp(buffer.data(), payload, sizeof(payload)), 0);
}
