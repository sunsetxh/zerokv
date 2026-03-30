#include "axon/config.h"
#include "axon/kv.h"
#include "kv/bench_utils.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace axon;
using namespace axon::kv;

namespace {

std::atomic<bool> g_stop{false};

void signal_handler(int) {
    g_stop.store(true);
}

void install_signal_handlers() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
}

void wait_until_stopped() {
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

std::vector<uint64_t> default_sizes() {
    const auto parsed = detail::parse_size_list("4K,64K,1M,4M,16M,32M,64M,128M");
    if (!parsed.ok()) {
        return {};
    }
    return parsed.value();
}

std::vector<std::byte> make_payload(uint64_t size_bytes) {
    std::vector<std::byte> payload(static_cast<size_t>(size_bytes));
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i % 251);
    }
    return payload;
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode;
    std::string listen_addr;
    std::string server_addr;
    std::string data_addr;
    std::string node_id;
    std::string sizes_arg;
    std::string transport = "tcp";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        } else if (arg == "--listen" && i + 1 < argc) {
            listen_addr = argv[++i];
        } else if (arg == "--server-addr" && i + 1 < argc) {
            server_addr = argv[++i];
        } else if (arg == "--data-addr" && i + 1 < argc) {
            data_addr = argv[++i];
        } else if (arg == "--node-id" && i + 1 < argc) {
            node_id = argv[++i];
        } else if (arg == "--sizes" && i + 1 < argc) {
            sizes_arg = argv[++i];
        } else if (arg == "--transport" && i + 1 < argc) {
            transport = argv[++i];
        }
    }

    install_signal_handlers();

    if (mode.empty()) {
        std::cerr << "Usage: " << argv[0]
                  << " --mode <server|hold-owner|bench-publish|bench-fetch>"
                  << " [--listen addr] [--server-addr addr] [--data-addr addr]"
                  << " [--node-id id] [--sizes list] [--transport tcp|rdma]\n";
        return 1;
    }

    auto cfg = Config::builder().set_transport(transport).build();

    if (mode == "server") {
        if (listen_addr.empty()) {
            std::cerr << "--listen is required for server mode\n";
            return 1;
        }

        auto server = KVServer::create(cfg);
        if (!server) {
            std::cerr << "Failed to create KVServer\n";
            return 1;
        }
        const auto status = server->start(ServerConfig{listen_addr});
        if (!status.ok()) {
            std::cerr << "Failed to start KVServer: " << status.message() << "\n";
            return 1;
        }

        std::cout << "kv_bench server listening on " << server->address() << "\n";
        wait_until_stopped();
        server->stop();
        return 0;
    }

    if (mode == "hold-owner") {
        if (server_addr.empty() || data_addr.empty()) {
            std::cerr << "--server-addr and --data-addr are required for hold-owner mode\n";
            return 1;
        }

        auto node = KVNode::create(cfg);
        if (!node) {
            std::cerr << "Failed to create KVNode\n";
            return 1;
        }
        const auto status = node->start(NodeConfig{
            .server_addr = server_addr,
            .local_data_addr = data_addr,
            .node_id = node_id,
        });
        if (!status.ok()) {
            std::cerr << "Failed to start KVNode: " << status.message() << "\n";
            return 1;
        }

        std::vector<uint64_t> sizes;
        if (sizes_arg.empty()) {
            sizes = default_sizes();
        } else {
            const auto parsed = detail::parse_size_list(sizes_arg);
            if (!parsed.ok()) {
                std::cerr << parsed.status.message() << "\n";
                node->stop();
                return 1;
            }
            sizes = parsed.value();
        }
        if (sizes.empty()) {
            std::cerr << "No benchmark sizes configured\n";
            node->stop();
            return 1;
        }

        for (const auto size_bytes : sizes) {
            auto payload = make_payload(size_bytes);
            const auto key = "bench-fetch-" + std::to_string(size_bytes);
            auto publish = node->publish(key, payload.data(), payload.size());
            if (!publish.status().ok()) {
                std::cerr << "hold-owner publish failed for " << key
                          << ": " << publish.status().message() << "\n";
                node->stop();
                return 1;
            }
            publish.get();
        }

        std::cout << "hold-owner ready: " << node->node_id() << "\n";
        wait_until_stopped();
        node->stop();
        return 0;
    }

    std::cerr << "Mode not implemented yet: " << mode << "\n";
    return 1;
}
