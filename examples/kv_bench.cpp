#include "axon/config.h"
#include "axon/kv.h"
#include "kv/bench_utils.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <optional>
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
    std::string owner_node_id;
    std::string sizes_arg;
    std::string transport = "tcp";
    std::string publish_api = "copy";
    std::optional<uint64_t> explicit_iters;
    uint64_t total_bytes = 1ull << 30;
    uint64_t warmup = 0;

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
        } else if (arg == "--owner-node-id" && i + 1 < argc) {
            owner_node_id = argv[++i];
        } else if (arg == "--sizes" && i + 1 < argc) {
            sizes_arg = argv[++i];
        } else if (arg == "--transport" && i + 1 < argc) {
            transport = argv[++i];
        } else if (arg == "--publish-api" && i + 1 < argc) {
            publish_api = argv[++i];
        } else if (arg == "--iters" && i + 1 < argc) {
            explicit_iters = std::stoull(argv[++i]);
        } else if (arg == "--total-bytes" && i + 1 < argc) {
            const auto parsed = detail::parse_size_list(argv[++i]);
            if (!parsed.ok() || parsed.value().size() != 1u) {
                std::cerr << "invalid --total-bytes value\n";
                return 1;
            }
            total_bytes = parsed.value().front();
        } else if (arg == "--warmup" && i + 1 < argc) {
            warmup = std::stoull(argv[++i]);
        }
    }

    install_signal_handlers();

    if (mode.empty()) {
        std::cerr << "Usage: " << argv[0]
                  << " --mode <server|hold-owner|bench-publish|bench-fetch|bench-fetch-to>"
                  << " [--listen addr] [--server-addr addr] [--data-addr addr]"
                  << " [--node-id id] [--owner-node-id id] [--sizes list]"
                  << " [--publish-api copy|region]"
                  << " [--iters N] [--total-bytes SIZE] [--warmup N] [--transport tcp|rdma]\n";
        return 1;
    }

    auto cfg = Config::builder().set_transport(transport).build();
    auto benchmark_ctx = Context::create(cfg);

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

    if (mode == "bench-publish") {
        if (server_addr.empty() || data_addr.empty()) {
            std::cerr << "--server-addr and --data-addr are required for bench-publish mode\n";
            return 1;
        }
        if (publish_api != "copy" && publish_api != "region") {
            std::cerr << "--publish-api must be one of: copy, region\n";
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

        std::vector<detail::PublishBenchRow> rows;
        for (const auto size_bytes : sizes) {
            const auto iterations = detail::derive_iterations(size_bytes, explicit_iters, total_bytes);
            auto payload = make_payload(size_bytes);
            axon::MemoryRegion::Ptr bench_region;
            if (publish_api == "region") {
                bench_region = axon::MemoryRegion::allocate(benchmark_ctx, payload.size());
                if (!bench_region) {
                    std::cerr << "publish-region benchmark failed to allocate region for size="
                              << size_bytes << "\n";
                    node->stop();
                    return 1;
                }
                std::memcpy(bench_region->address(), payload.data(), payload.size());
            }

            uint64_t total_sum = 0;
            uint64_t prepare_sum = 0;
            uint64_t pack_sum = 0;
            uint64_t rpc_sum = 0;

            for (uint64_t i = 0; i < warmup; ++i) {
                const auto key = "bench-publish-warmup-" + std::to_string(size_bytes) + "-" + std::to_string(i);
                auto publish = (publish_api == "region")
                    ? node->publish_region(key, bench_region, payload.size())
                    : node->publish(key, payload.data(), payload.size());
                if (!publish.status().ok()) {
                    std::cerr << "publish benchmark warmup failed: size=" << size_bytes
                              << " iter=" << i << " error=" << publish.status().message() << "\n";
                    node->stop();
                    return 1;
                }
                publish.get();

                auto unpublish = node->unpublish(key);
                if (!unpublish.status().ok()) {
                    std::cerr << "unpublish warmup failed: " << unpublish.status().message() << "\n";
                    node->stop();
                    return 1;
                }
                unpublish.get();
            }

            for (uint64_t i = 0; i < iterations; ++i) {
                const auto key = "bench-publish-" + std::to_string(size_bytes) + "-" + std::to_string(i);
                auto publish = (publish_api == "region")
                    ? node->publish_region(key, bench_region, payload.size())
                    : node->publish(key, payload.data(), payload.size());
                if (!publish.status().ok()) {
                    std::cerr << "publish benchmark failed: size=" << size_bytes
                              << " iter=" << i << " error=" << publish.status().message() << "\n";
                    node->stop();
                    return 1;
                }
                publish.get();

                const auto metrics = node->last_publish_metrics();
                if (!metrics.has_value() || !metrics->ok) {
                    std::cerr << "missing publish metrics for size=" << size_bytes
                              << " iter=" << i << "\n";
                    node->stop();
                    return 1;
                }

                total_sum += metrics->total_us;
                prepare_sum += metrics->prepare_region_us;
                pack_sum += metrics->pack_rkey_us;
                rpc_sum += metrics->put_meta_rpc_us;

                auto unpublish = node->unpublish(key);
                if (!unpublish.status().ok()) {
                    std::cerr << "unpublish failed: " << unpublish.status().message() << "\n";
                    node->stop();
                    return 1;
                }
                unpublish.get();
            }

            rows.push_back(detail::PublishBenchRow{
                .size_bytes = size_bytes,
                .iterations = iterations,
                .avg_total_us = static_cast<double>(total_sum) / static_cast<double>(iterations),
                .avg_prepare_us = static_cast<double>(prepare_sum) / static_cast<double>(iterations),
                .avg_pack_rkey_us = static_cast<double>(pack_sum) / static_cast<double>(iterations),
                .avg_put_meta_rpc_us = static_cast<double>(rpc_sum) / static_cast<double>(iterations),
                .throughput_MiBps = detail::throughput_mb_per_sec(
                    size_bytes,
                    static_cast<double>(total_sum) / static_cast<double>(iterations)),
            });
        }

        std::cout << "op=publish transport=" << transport
                  << " node_id=" << node->node_id()
                  << " publish_api=" << publish_api
                  << " total_bytes=" << total_bytes
                  << " warmup=" << warmup;
        if (explicit_iters.has_value()) {
            std::cout << " iters=" << *explicit_iters;
        }
        std::cout << "\n";
        std::cout << detail::render_publish_rows(rows);
        node->stop();
        return 0;
    }

    if (mode == "bench-fetch" || mode == "bench-fetch-to") {
        if (server_addr.empty() || data_addr.empty()) {
            std::cerr << "--server-addr and --data-addr are required for fetch benchmark modes\n";
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

        std::vector<detail::FetchBenchRow> rows;
        for (const auto size_bytes : sizes) {
            const auto iterations = detail::derive_iterations(size_bytes, explicit_iters, total_bytes);
            const auto key = "bench-fetch-" + std::to_string(size_bytes);
            axon::MemoryRegion::Ptr local_region;
            if (mode == "bench-fetch-to") {
                local_region = axon::MemoryRegion::allocate(benchmark_ctx, size_bytes);
                if (!local_region) {
                    std::cerr << "fetch-to benchmark failed to allocate local region for size="
                              << size_bytes << "\n";
                    node->stop();
                    return 1;
                }
            }

            uint64_t total_sum = 0;
            uint64_t prepare_sum = 0;
            uint64_t meta_sum = 0;
            uint64_t connect_sum = 0;
            uint64_t rkey_prepare_sum = 0;
            uint64_t get_submit_sum = 0;
            uint64_t rdma_prepare_sum = 0;
            uint64_t rdma_get_sum = 0;

            for (uint64_t i = 0; i < warmup; ++i) {
                if (mode == "bench-fetch-to") {
                    auto fetch = node->fetch_to(key, local_region, size_bytes, 0);
                    if (!fetch.status().ok()) {
                        std::cerr << "fetch-to benchmark warmup failed: size=" << size_bytes
                                  << " iter=" << i << " error=" << fetch.status().message() << "\n";
                        node->stop();
                        return 1;
                    }
                    fetch.get();
                } else {
                    auto fetch = node->fetch(key);
                    if (!fetch.status().ok()) {
                        std::cerr << "fetch benchmark warmup failed: size=" << size_bytes
                                  << " iter=" << i << " error=" << fetch.status().message() << "\n";
                        node->stop();
                        return 1;
                    }
                    const auto result = fetch.get();
                    if (result.data.size() != size_bytes) {
                        std::cerr << "fetch benchmark warmup size mismatch: expected=" << size_bytes
                                  << " actual=" << result.data.size() << "\n";
                        node->stop();
                        return 1;
                    }
                    if (!owner_node_id.empty() && result.owner_node_id != owner_node_id) {
                        std::cerr << "fetch benchmark warmup owner mismatch: expected=" << owner_node_id
                                  << " actual=" << result.owner_node_id << "\n";
                        node->stop();
                        return 1;
                    }
                }
            }

            for (uint64_t i = 0; i < iterations; ++i) {
                if (mode == "bench-fetch-to") {
                    auto fetch = node->fetch_to(key, local_region, size_bytes, 0);
                    if (!fetch.status().ok()) {
                        std::cerr << "fetch-to benchmark failed: size=" << size_bytes
                                  << " iter=" << i << " error=" << fetch.status().message() << "\n";
                        node->stop();
                        return 1;
                    }
                    fetch.get();
                } else {
                    auto fetch = node->fetch(key);
                    if (!fetch.status().ok()) {
                        std::cerr << "fetch benchmark failed: size=" << size_bytes
                                  << " iter=" << i << " error=" << fetch.status().message() << "\n";
                        node->stop();
                        return 1;
                    }
                    const auto result = fetch.get();
                    if (result.data.size() != size_bytes) {
                        std::cerr << "fetch benchmark size mismatch: expected=" << size_bytes
                                  << " actual=" << result.data.size() << "\n";
                        node->stop();
                        return 1;
                    }
                    if (!owner_node_id.empty() && result.owner_node_id != owner_node_id) {
                        std::cerr << "fetch benchmark owner mismatch: expected=" << owner_node_id
                                  << " actual=" << result.owner_node_id << "\n";
                        node->stop();
                        return 1;
                    }
                }

                const auto metrics = node->last_fetch_metrics();
                if (!metrics.has_value() || !metrics->ok) {
                    std::cerr << "missing fetch metrics for size=" << size_bytes
                              << " iter=" << i << "\n";
                    node->stop();
                    return 1;
                }

                total_sum += metrics->total_us;
                prepare_sum += metrics->local_buffer_prepare_us;
                meta_sum += metrics->get_meta_rpc_us;
                connect_sum += metrics->peer_connect_us;
                rkey_prepare_sum += metrics->rkey_prepare_us;
                get_submit_sum += metrics->get_submit_us;
                rdma_prepare_sum += metrics->rdma_prepare_us;
                rdma_get_sum += metrics->rdma_get_us;
            }

            rows.push_back(detail::FetchBenchRow{
                .size_bytes = size_bytes,
                .iterations = iterations,
                .avg_total_us = static_cast<double>(total_sum) / static_cast<double>(iterations),
                .avg_prepare_us = static_cast<double>(prepare_sum) / static_cast<double>(iterations),
                .avg_get_meta_rpc_us = static_cast<double>(meta_sum) / static_cast<double>(iterations),
                .avg_peer_connect_us = static_cast<double>(connect_sum) / static_cast<double>(iterations),
                .avg_rkey_prepare_us = static_cast<double>(rkey_prepare_sum) / static_cast<double>(iterations),
                .avg_get_submit_us = static_cast<double>(get_submit_sum) / static_cast<double>(iterations),
                .avg_rdma_prepare_us = static_cast<double>(rdma_prepare_sum) / static_cast<double>(iterations),
                .avg_rdma_get_us = static_cast<double>(rdma_get_sum) / static_cast<double>(iterations),
                .throughput_MiBps = detail::throughput_mb_per_sec(
                    size_bytes,
                    static_cast<double>(total_sum) / static_cast<double>(iterations)),
            });
        }

        std::cout << "op=" << (mode == "bench-fetch-to" ? "fetch_to" : "fetch")
                  << " transport=" << transport
                  << " node_id=" << node->node_id()
                  << " total_bytes=" << total_bytes
                  << " warmup=" << warmup;
        if (!owner_node_id.empty()) {
            std::cout << " owner_node_id=" << owner_node_id;
        }
        if (explicit_iters.has_value()) {
            std::cout << " iters=" << *explicit_iters;
        }
        std::cout << "\n";
        std::cout << detail::render_fetch_rows(rows);
        node->stop();
        return 0;
    }

    std::cerr << "Mode not implemented yet: " << mode << "\n";
    return 1;
}
