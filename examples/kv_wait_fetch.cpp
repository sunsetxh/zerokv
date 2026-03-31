#include <zerokv/config.h>
#include <zerokv/kv.h>

#include <chrono>
#include <cstddef>
#include <iostream>
#include <string>

using namespace axon;
using namespace axon::kv;

namespace {

void print_usage(const char* argv0) {
    std::cerr << "Usage: " << argv0
              << " --mode <wait-for-key|subscribe-fetch-once>"
              << " --server-addr <addr> --data-addr <addr> --node-id <id> --key <key>"
              << " [--timeout-ms <ms>] [--transport tcp|rdma]\n";
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

}  // namespace

int main(int argc, char** argv) {
    std::string mode;
    std::string server_addr;
    std::string data_addr;
    std::string node_id;
    std::string key;
    std::string transport = "tcp";
    int timeout_ms = 5000;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        } else if (arg == "--server-addr" && i + 1 < argc) {
            server_addr = argv[++i];
        } else if (arg == "--data-addr" && i + 1 < argc) {
            data_addr = argv[++i];
        } else if (arg == "--node-id" && i + 1 < argc) {
            node_id = argv[++i];
        } else if (arg == "--key" && i + 1 < argc) {
            key = argv[++i];
        } else if (arg == "--timeout-ms" && i + 1 < argc) {
            timeout_ms = std::stoi(argv[++i]);
        } else if (arg == "--transport" && i + 1 < argc) {
            transport = argv[++i];
        }
    }

    if (mode.empty() || server_addr.empty() || data_addr.empty() || node_id.empty() || key.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    auto cfg = Config::builder().set_transport(transport).build();
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

    try {
        const auto timeout = std::chrono::milliseconds(timeout_ms);
        if (mode == "wait-for-key") {
            auto status = node->wait_for_key(key, timeout);
            if (!status.ok()) {
                std::cerr << "WAIT_ERR key=" << key << " message=" << status.message() << "\n";
                node->stop();
                return 1;
            }
            std::cout << "WAIT_OK key=" << key << "\n";
            node->stop();
            return 0;
        }

        if (mode == "subscribe-fetch-once") {
            auto result = node->subscribe_and_fetch_once(key, timeout);
            auto value = std::string(reinterpret_cast<const char*>(result.data.data()), result.data.size());
            std::cout << "FETCH_OK key=" << key
                      << " owner=" << result.owner_node_id
                      << " version=" << result.version
                      << " value=" << value << "\n";
            print_fetch_metrics(node);
            node->stop();
            return 0;
        }

        std::cerr << "Unknown mode: " << mode << "\n";
        print_usage(argv[0]);
        node->stop();
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "WAIT_FETCH_ERR key=" << key << " message=" << ex.what() << "\n";
        node->stop();
        return 1;
    }
}
