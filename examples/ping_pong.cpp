/// @file ping_pong.cpp
/// @brief Tag-matched ping-pong latency benchmark over RDMA or TCP.
///
/// Usage:
///   Server: ./ping_pong --listen 0.0.0.0:13337
///   Client: ./ping_pong --connect 10.0.0.1:13337
///
/// Transport is controlled via --transport flag (default: tcp).
/// For RDMA: ./ping_pong --transport rdma --connect 10.0.0.1:13337

#include <zerokv/config.h>
#include <zerokv/worker.h>
#include <zerokv/endpoint.h>

#include <iostream>
#include <vector>
#include <cstring>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>

using namespace zerokv;

constexpr size_t kMessageSize = 4096;
constexpr int kNumIterations = 1000;
constexpr Tag kPingTag = make_tag(1, 1);
constexpr Tag kPongTag = make_tag(1, 2);

// Global endpoint for server callback
static std::shared_ptr<Endpoint> g_server_ep;
static std::mutex g_ep_mutex;
static std::condition_variable g_ep_cv;

int main(int argc, char** argv) {
    std::string mode;
    std::string address;
    std::string transport = "tcp";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--listen" && i + 1 < argc) {
            mode = "server";
            address = argv[++i];
        } else if (arg == "--connect" && i + 1 < argc) {
            mode = "client";
            address = argv[++i];
        } else if (arg == "--transport" && i + 1 < argc) {
            transport = argv[++i];
        }
    }

    if (mode.empty()) {
        std::cerr << "Usage: " << argv[0] << " --listen <addr> | --connect <addr> [--transport tcp|rdma]\n";
        return 1;
    }

    auto config = Config::builder()
        .set_transport(transport)
        .build();

    auto ctx = Context::create(config);
    if (!ctx) {
        std::cerr << "Failed to create context\n";
        return 1;
    }

    auto worker = Worker::create(ctx);
    if (!worker) {
        std::cerr << "Failed to create worker\n";
        return 1;
    }

    std::cout << "Transport: " << transport << "\n";

    if (mode == "server") {
        // Server: listen → accept → pong loop
        std::cout << "Listening on " << address << "...\n";

        auto listener = worker->listen(address, [](Endpoint::Ptr ep) {
            std::lock_guard<std::mutex> lock(g_ep_mutex);
            g_server_ep = ep;
            g_ep_cv.notify_one();
            std::cout << "Client connected!\n";
        });

        if (!listener) {
            std::cerr << "Failed to create listener\n";
            return 1;
        }

        std::cout << "Listening on " << listener->address() << "\n";

        // Wait for connection via polling
        {
            while (!g_server_ep) {
                worker->progress();
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }

        auto ep = g_server_ep;
        std::vector<char> buffer(kMessageSize);

        // Server pong loop: recv ping → send pong
        for (int i = 0; i < kNumIterations; ++i) {
            auto recv_fut = ep->tag_recv(buffer.data(), buffer.size(), kPingTag);
            recv_fut.get();

            auto send_fut = ep->tag_send(buffer.data(), buffer.size(), kPongTag);
            send_fut.get();
        }

        std::cout << "Server completed " << kNumIterations << " ping-pong iterations\n";

    } else {
        // Client: connect → ping loop
        std::cout << "Connecting to " << address << "...\n";

        auto connect_fut = worker->connect(address);
        auto ep = connect_fut.get();
        if (!ep) {
            std::cerr << "Failed to connect\n";
            return 1;
        }
        std::cout << "Connected!\n";

        // Flush to ensure connection is fully established
        {
            auto flush_fut = ep->flush();
            flush_fut.get();
        }

        std::vector<char> send_buf(kMessageSize, 'X');
        std::vector<char> recv_buf(kMessageSize);

        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < kNumIterations; ++i) {
            // Send ping
            auto send_fut = ep->tag_send(send_buf.data(), send_buf.size(), kPingTag);
            send_fut.get();

            // Recv pong
            auto recv_fut = ep->tag_recv(recv_buf.data(), recv_buf.size(), kPongTag);
            recv_fut.get();
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        double latency_us = static_cast<double>(total_us) / kNumIterations;

        std::cout << "Completed " << kNumIterations << " iterations\n";
        std::cout << "Total time: " << total_us << " us\n";
        std::cout << "Avg round-trip latency: " << latency_us << " us ("
                  << (latency_us / 2.0) << " us one-way)\n";
        double throughput_mbps = (kMessageSize * 2.0 * kNumIterations) / (total_us * 1.0);
        std::cout << "Throughput: " << throughput_mbps << " MB/s\n";
    }

    return 0;
}
