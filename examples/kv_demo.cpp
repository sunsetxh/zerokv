/// @file kv_demo.cpp
/// @brief Minimal KV MVP demo: server, publisher, reader.
///
/// Usage:
///   Server:
///     ./kv_demo --mode server --listen 10.0.0.1:15000
///   Publisher:
///     ./kv_demo --mode publish --server-addr 10.0.0.1:15000 --data-addr 10.0.0.1:0
///               --node-id publisher --key mykey --value hello --hold
///   Reader:
///     ./kv_demo --mode fetch --server-addr 10.0.0.1:15000 --data-addr 10.0.0.2:0
///               --node-id reader --key mykey
///   Pusher:
///     ./kv_demo --mode push --server-addr 10.0.0.1:15000 --data-addr 10.0.0.2:0
///               --node-id sender --target-node-id target --key mykey --value hello
///
/// Known issue:
///   With UCX 1.20 on Soft-RoCE (`rxe0`) across QEMU VMs, the UCX new protocol
///   stack may hit an internal assertion in `proto_select.c` during KV fetch.
///   Workaround for this environment:
///     UCX_PROTO_ENABLE=n UCX_NET_DEVICES=rxe0:1 ./kv_demo --mode fetch ...
///   This workaround was not needed for single-VM testing and should not be
///   treated as a general ZeroKV library requirement.

#include <zerokv/kv.h>
#include <zerokv/config.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

using namespace zerokv;
using namespace zerokv::core;

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

void print_publish_metrics(const KVNode::Ptr& node) {
    if (!node) {
        return;
    }
    auto metrics = node->last_publish_metrics();
    if (!metrics.has_value()) {
        return;
    }
    std::cout << "publish_metrics"
              << " total_us=" << metrics->total_us
              << " prepare_region_us=" << metrics->prepare_region_us
              << " pack_rkey_us=" << metrics->pack_rkey_us
              << " put_meta_rpc_us=" << metrics->put_meta_rpc_us
              << " ok=" << (metrics->ok ? 1 : 0) << "\n";
}

void print_fetch_metrics(const KVNode::Ptr& node) {
    if (!node) {
        return;
    }
    auto metrics = node->last_fetch_metrics();
    if (!metrics.has_value()) {
        return;
    }
    std::cout << "fetch_metrics"
              << " total_us=" << metrics->total_us
              << " local_buffer_prepare_us=" << metrics->local_buffer_prepare_us
              << " get_meta_rpc_us=" << metrics->get_meta_rpc_us
              << " peer_connect_us=" << metrics->peer_connect_us
              << " rdma_prepare_us=" << metrics->rdma_prepare_us
              << " rdma_get_us=" << metrics->rdma_get_us
              << " ok=" << (metrics->ok ? 1 : 0) << "\n";
}

void print_push_metrics(const KVNode::Ptr& node) {
    if (!node) {
        return;
    }
    auto metrics = node->last_push_metrics();
    if (!metrics.has_value()) {
        return;
    }
    std::cout << "push_metrics"
              << " total_us=" << metrics->total_us
              << " get_target_rpc_us=" << metrics->get_target_rpc_us
              << " prepare_frame_us=" << metrics->prepare_frame_us
              << " rdma_put_flush_us=" << metrics->rdma_put_flush_us
              << " commit_rpc_us=" << metrics->commit_rpc_us
              << " ok=" << (metrics->ok ? 1 : 0) << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode;
    std::string listen_addr;
    std::string server_addr;
    std::string data_addr;
    std::string node_id;
    std::string key;
    std::string value;
    std::string target_node_id;
    std::string transport = "tcp";
    bool hold = false;

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
        } else if (arg == "--key" && i + 1 < argc) {
            key = argv[++i];
        } else if (arg == "--value" && i + 1 < argc) {
            value = argv[++i];
        } else if (arg == "--target-node-id" && i + 1 < argc) {
            target_node_id = argv[++i];
        } else if (arg == "--transport" && i + 1 < argc) {
            transport = argv[++i];
        } else if (arg == "--hold") {
            hold = true;
        }
    }

    if (mode.empty()) {
        std::cerr << "Usage: " << argv[0]
                  << " --mode <server|publish|fetch|push|unpublish>"
                  << " [--listen addr] [--server-addr addr] [--data-addr addr]"
                  << " [--node-id id] [--target-node-id id] [--key key] [--value value]"
                  << " [--transport tcp|rdma] [--hold]\n";
        return 1;
    }

    install_signal_handlers();

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
        auto status = server->start(ServerConfig{listen_addr});
        if (!status.ok()) {
            std::cerr << "Failed to start KVServer: " << status.message() << "\n";
            return 1;
        }

        std::cout << "KV server listening on " << server->address() << "\n";
        wait_until_stopped();
        server->stop();
        return 0;
    }

    if (server_addr.empty() || data_addr.empty() || key.empty()) {
        std::cerr << "--server-addr, --data-addr, and --key are required for node modes\n";
        return 1;
    }

    auto node = KVNode::create(cfg);
    if (!node) {
        std::cerr << "Failed to create KVNode\n";
        return 1;
    }

    auto start = node->start(NodeConfig{
        .server_addr = server_addr,
        .local_data_addr = data_addr,
        .node_id = node_id,
    });
    if (!start.ok()) {
        std::cerr << "Failed to start KVNode: " << start.message() << "\n";
        return 1;
    }

    std::cout << "KV node started: " << node->node_id() << "\n";

    if (mode == "publish") {
        if (value.empty()) {
            std::cerr << "--value is required for publish mode\n";
            node->stop();
            return 1;
        }

        auto publish = node->publish(key, value.data(), value.size());
        if (!publish.status().ok()) {
            std::cerr << "Publish failed: " << publish.status().message() << "\n";
            node->stop();
            return 1;
        }
        publish.get();
        std::cout << "Published key=" << key << " bytes=" << value.size() << "\n";
        print_publish_metrics(node);

        if (hold) {
            std::cout << "Holding publisher alive. Press Ctrl+C to exit.\n";
            wait_until_stopped();
        }
        node->stop();
        return 0;
    }

    if (mode == "push") {
        if (value.empty() || target_node_id.empty()) {
            std::cerr << "--value and --target-node-id are required for push mode\n";
            node->stop();
            return 1;
        }

        auto push = node->push(target_node_id, key, value.data(), value.size());
        if (!push.status().ok()) {
            std::cerr << "Push failed: " << push.status().message() << "\n";
            node->stop();
            return 1;
        }
        push.get();
        std::cout << "Pushed key=" << key
                  << " target=" << target_node_id
                  << " bytes=" << value.size() << "\n";
        print_push_metrics(node);
        node->stop();
        return 0;
    }

    if (mode == "fetch") {
        auto fetch = node->fetch(key);
        if (!fetch.status().ok()) {
            std::cerr << "Fetch failed: " << fetch.status().message() << "\n";
            node->stop();
            return 1;
        }
        auto result = fetch.get();
        std::string fetched(result.data.size(), '\0');
        if (!result.data.empty()) {
            std::memcpy(fetched.data(), result.data.data(), result.data.size());
        }
        std::cout << "Fetched key=" << key
                  << " owner=" << result.owner_node_id
                  << " version=" << result.version
                  << " value=" << fetched << "\n";
        print_fetch_metrics(node);
        node->stop();
        return 0;
    }

    if (mode == "unpublish") {
        auto unpublish = node->unpublish(key);
        if (!unpublish.status().ok()) {
            std::cerr << "Unpublish failed: " << unpublish.status().message() << "\n";
            node->stop();
            return 1;
        }
        unpublish.get();
        std::cout << "Unpublished key=" << key << "\n";
        node->stop();
        return 0;
    }

    std::cerr << "Unknown mode: " << mode << "\n";
    node->stop();
    return 1;
}
