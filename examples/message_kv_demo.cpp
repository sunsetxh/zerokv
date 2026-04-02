#include <zerokv/config.h>
#include <zerokv/kv.h>
#include <zerokv/message_kv.h>

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace zerokv;
using namespace zerokv::kv;

namespace zerokv::examples::message_kv_demo {

namespace {

std::vector<size_t> parse_sizes_impl(const std::string& csv) {
    std::vector<size_t> sizes;
    std::stringstream stream(csv);
    std::string token;

    while (std::getline(stream, token, ',')) {
        token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        }), token.end());
        if (token.empty()) {
            throw std::invalid_argument("empty benchmark size token");
        }

        size_t value_end = 0;
        size_t value = 0;
        try {
            value = std::stoull(token, &value_end);
        } catch (const std::exception&) {
            throw std::invalid_argument("invalid benchmark size token: " + token);
        }

        std::string suffix = token.substr(value_end);
        std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });

        size_t scale = 1;
        if (suffix.empty()) {
            scale = 1;
        } else if (suffix == "K" || suffix == "KB" || suffix == "KIB") {
            scale = 1024u;
        } else if (suffix == "M" || suffix == "MB" || suffix == "MIB") {
            scale = 1024u * 1024u;
        } else if (suffix == "G" || suffix == "GB" || suffix == "GIB") {
            scale = 1024u * 1024u * 1024u;
        } else {
            throw std::invalid_argument("invalid benchmark size suffix: " + suffix);
        }

        sizes.push_back(value * scale);
    }

    if (sizes.empty()) {
        throw std::invalid_argument("benchmark size list is empty");
    }

    return sizes;
}

}  // namespace

std::vector<size_t> parse_sizes_csv(const std::string& csv) {
    return parse_sizes_impl(csv);
}

std::string make_round_key(size_t round_index, size_t size_bytes, size_t thread_index) {
    return "msg-round" + std::to_string(round_index) + "-size" +
           std::to_string(size_bytes) + "-thread" + std::to_string(thread_index);
}

std::string make_payload(size_t round_index, size_t size_bytes, size_t thread_index) {
    std::string payload(size_bytes, 'x');
    const std::string prefix = "round" + std::to_string(round_index) + "-thread" +
                               std::to_string(thread_index) + "-";
    const auto copy_len = std::min(prefix.size(), payload.size());
    std::memcpy(payload.data(), prefix.data(), copy_len);
    return payload;
}

size_t max_size_bytes_for_sizes(const std::vector<size_t>& sizes) {
    return *std::max_element(sizes.begin(), sizes.end());
}

double throughput_mib_per_sec(size_t bytes, uint64_t elapsed_us_value) {
    if (elapsed_us_value == 0) {
        return 0.0;
    }
    const double seconds = static_cast<double>(elapsed_us_value) / 1000000.0;
    return static_cast<double>(bytes) / (1024.0 * 1024.0) / seconds;
}

std::string render_send_round_summary(size_t round_index,
                                      size_t size_bytes,
                                      size_t messages,
                                      uint64_t send_total_us,
                                      uint64_t max_thread_send_us,
                                      size_t total_bytes) {
    std::ostringstream out;
    out << "SEND_ROUND round=" << round_index
        << " size=" << size_bytes
        << " messages=" << messages
        << " send_total_us=" << send_total_us
        << " max_thread_send_us=" << max_thread_send_us
        << " total_bytes=" << total_bytes
        << " throughput_MiBps=" << throughput_mib_per_sec(total_bytes, send_total_us);
    return out.str();
}

std::string make_compact_preview(const void* data, size_t size_bytes, size_t max_bytes) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    const size_t preview_bytes = std::min(size_bytes, max_bytes);
    std::string preview;
    preview.reserve(preview_bytes + (size_bytes > max_bytes ? 3 : 0));
    for (size_t i = 0; i < preview_bytes; ++i) {
        const unsigned char ch = bytes[i];
        preview.push_back(std::isprint(ch) ? static_cast<char>(ch) : '.');
    }
    if (size_bytes > max_bytes) {
        preview += "...";
    }
    return preview;
}

std::string render_recv_round_summary(size_t round_index,
                                      size_t size_bytes,
                                      size_t completed,
                                      size_t failed,
                                      size_t timed_out,
                                      bool completed_all,
                                      uint64_t recv_total_us,
                                      size_t total_bytes) {
    std::ostringstream out;
    out << "RECV_ROUND round=" << round_index
        << " size=" << size_bytes
        << " completed=" << completed
        << " failed=" << failed
        << " timed_out=" << timed_out
        << " completed_all=" << (completed_all ? 1 : 0)
        << " recv_total_us=" << recv_total_us
        << " total_bytes=" << total_bytes
        << " throughput_MiBps=" << throughput_mib_per_sec(total_bytes, recv_total_us);
    return out.str();
}

}  // namespace zerokv::examples::message_kv_demo

#ifndef MESSAGE_KV_DEMO_BUILD_TESTS

namespace {

using SteadyClock = std::chrono::steady_clock;

struct Args {
    std::string role;
    std::string listen_addr;
    std::string server_addr;
    std::string data_addr;
    std::string node_id = "rank0";
    std::string transport = "tcp";
    std::string sizes_csv = "1K,64K,1M,4M,16M,32M,64M,128M";
    int threads = 4;
    int messages = 4;
    int timeout_ms = 5000;
    int post_recv_wait_ms = 2000;
    int warmup_rounds = 1;
};

void print_usage(const char* argv0) {
    std::cerr
        << "Usage:\n"
        << "  " << argv0
        << " --role rank0 --listen <addr> --data-addr <addr> --node-id <id>\n"
        << "           [--messages 4] [--timeout-ms 5000] [--post-recv-wait-ms 2000]"
        << " [--warmup-rounds 1] [--sizes 1K,64K,...] [--transport tcp|rdma]\n"
        << "  " << argv0
        << " --role rank1 --server-addr <addr> --data-addr <addr> --node-id <id>\n"
        << "           [--threads 4] [--warmup-rounds 1] [--sizes 1K,64K,...] [--timeout-ms 5000]"
        << " [--transport tcp|rdma]\n";
}

bool parse_args(int argc, char** argv, Args* args) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--role" && i + 1 < argc) {
            args->role = argv[++i];
        } else if (arg == "--listen" && i + 1 < argc) {
            args->listen_addr = argv[++i];
        } else if (arg == "--server-addr" && i + 1 < argc) {
            args->server_addr = argv[++i];
        } else if (arg == "--data-addr" && i + 1 < argc) {
            args->data_addr = argv[++i];
        } else if (arg == "--node-id" && i + 1 < argc) {
            args->node_id = argv[++i];
        } else if (arg == "--transport" && i + 1 < argc) {
            args->transport = argv[++i];
        } else if (arg == "--sizes" && i + 1 < argc) {
            args->sizes_csv = argv[++i];
        } else if (arg == "--threads" && i + 1 < argc) {
            args->threads = std::stoi(argv[++i]);
        } else if (arg == "--messages" && i + 1 < argc) {
            args->messages = std::stoi(argv[++i]);
        } else if (arg == "--timeout-ms" && i + 1 < argc) {
            args->timeout_ms = std::stoi(argv[++i]);
        } else if (arg == "--post-recv-wait-ms" && i + 1 < argc) {
            args->post_recv_wait_ms = std::stoi(argv[++i]);
        } else if (arg == "--warmup-rounds" && i + 1 < argc) {
            args->warmup_rounds = std::stoi(argv[++i]);
        } else {
            return false;
        }
    }
    return true;
}

std::string make_sender_node_id(const std::string& base,
                                size_t round_index,
                                int thread_index) {
    return base + "-round" + std::to_string(round_index) +
           "-thread" + std::to_string(thread_index);
}

uint64_t elapsed_us(SteadyClock::time_point start, SteadyClock::time_point end) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

int run_rank0(const Args& args, const Config& cfg) {
    if (args.listen_addr.empty() || args.data_addr.empty() || args.node_id.empty()) {
        std::cerr << "--listen, --data-addr, and --node-id are required for rank0\n";
        return 1;
    }
    if (args.messages <= 0) {
        std::cerr << "--messages must be > 0\n";
        return 1;
    }
    if (args.post_recv_wait_ms < 0) {
        std::cerr << "--post-recv-wait-ms must be >= 0\n";
        return 1;
    }
    if (args.warmup_rounds < 0) {
        std::cerr << "--warmup-rounds must be >= 0\n";
        return 1;
    }

    std::vector<size_t> sizes;
    try {
        sizes = zerokv::examples::message_kv_demo::parse_sizes_csv(args.sizes_csv);
    } catch (const std::exception& ex) {
        std::cerr << "--sizes: " << ex.what() << "\n";
        return 1;
    }

    auto server = KVServer::create(cfg);
    if (!server) {
        std::cerr << "failed to create KVServer\n";
        return 1;
    }
    auto server_status = server->start(ServerConfig{args.listen_addr});
    if (!server_status.ok()) {
        std::cerr << "failed to start server: " << server_status.message() << "\n";
        return 1;
    }

    auto ctx = Context::create(cfg);
    if (!ctx) {
        std::cerr << "failed to create Context\n";
        server->stop();
        return 1;
    }

    auto mq = MessageKV::create(cfg);
    try {
        mq->start(NodeConfig{
            .server_addr = server->address(),
            .local_data_addr = args.data_addr,
            .node_id = args.node_id,
        });
    } catch (const std::exception& ex) {
        std::cerr << "failed to start rank0 receiver: " << ex.what() << "\n";
        server->stop();
        return 1;
    }

    auto run_round = [&](size_t protocol_round_index,
                         size_t report_round_index,
                         size_t size_bytes,
                         bool print_summary) -> bool {
        const size_t total_bytes = size_bytes * static_cast<size_t>(args.messages);

        std::vector<MessageKV::BatchRecvItem> items;
        std::vector<std::string> keys;
        std::vector<std::string> payloads;
        items.reserve(static_cast<size_t>(args.messages));
        keys.reserve(static_cast<size_t>(args.messages));
        payloads.reserve(static_cast<size_t>(args.messages));

        for (int i = 0; i < args.messages; ++i) {
            const auto key = zerokv::examples::message_kv_demo::make_round_key(
                protocol_round_index, size_bytes, static_cast<size_t>(i));
            const auto payload = zerokv::examples::message_kv_demo::make_payload(
                protocol_round_index, size_bytes, static_cast<size_t>(i));
            keys.push_back(key);
            payloads.push_back(payload);
            items.push_back(MessageKV::BatchRecvItem{
                .key = key,
                .length = size_bytes,
                .offset = static_cast<size_t>(i) * size_bytes,
            });
        }

        auto region = MemoryRegion::allocate(ctx, total_bytes);
        if (!region) {
            std::cerr << "failed to allocate receive region\n";
            mq->stop();
            server->stop();
            return 1;
        }

        try {
            const auto recv_start = SteadyClock::now();
            const auto result =
                mq->recv_batch(items, region, std::chrono::milliseconds(args.timeout_ms));
            const auto recv_end = SteadyClock::now();
            const auto recv_us = elapsed_us(recv_start, recv_end);

            if (print_summary) {
                std::cout << zerokv::examples::message_kv_demo::render_recv_round_summary(
                                 report_round_index,
                                 size_bytes,
                                 result.completed.size(),
                                 result.failed.size(),
                                 result.timed_out.size(),
                                 result.completed_all,
                                 recv_us,
                                 total_bytes)
                          << "\n";
            }

            if (!result.completed_all || !result.failed.empty() || !result.timed_out.empty()) {
                return false;
            }

            if (print_summary) {
                for (int i = 0; i < args.messages; ++i) {
                    const size_t offset = static_cast<size_t>(i) * size_bytes;
                    const auto* data = static_cast<const char*>(region->address()) + offset;
                    const auto& expected_payload = payloads[static_cast<size_t>(i)];
                    if (std::memcmp(data, expected_payload.data(), expected_payload.size()) != 0) {
                        throw std::runtime_error("received payload mismatch for key " +
                                                 keys[static_cast<size_t>(i)]);
                    }
                    std::cout << "RECV_OK key=" << keys[static_cast<size_t>(i)]
                              << " bytes=" << size_bytes
                              << " preview="
                              << zerokv::examples::message_kv_demo::make_compact_preview(
                                     expected_payload.data(), expected_payload.size(), 24)
                              << "\n";
                }
            }
            return true;
        } catch (const std::exception& ex) {
            std::cerr << "RECV_ERR message=" << ex.what() << "\n";
            return false;
        }
    };

    size_t protocol_round_index = 0;
    for (int warmup = 0; warmup < args.warmup_rounds; ++warmup, ++protocol_round_index) {
        if (!run_round(protocol_round_index, 0, sizes.front(), false)) {
            mq->stop();
            server->stop();
            return 1;
        }
    }

    for (size_t round_index = 0; round_index < sizes.size(); ++round_index, ++protocol_round_index) {
        if (!run_round(protocol_round_index, round_index, sizes[round_index], true)) {
            mq->stop();
            server->stop();
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(args.post_recv_wait_ms));
    }

    mq->stop();
    server->stop();
    return 0;
}

int run_rank1(const Args& args, const Config& cfg) {
    if (args.server_addr.empty() || args.data_addr.empty() || args.node_id.empty()) {
        std::cerr << "--server-addr, --data-addr, and --node-id are required for rank1\n";
        return 1;
    }
    if (args.threads <= 0) {
        std::cerr << "--threads must be > 0\n";
        return 1;
    }
    if (args.warmup_rounds < 0) {
        std::cerr << "--warmup-rounds must be >= 0\n";
        return 1;
    }
    std::vector<size_t> sizes;
    try {
        sizes = zerokv::examples::message_kv_demo::parse_sizes_csv(args.sizes_csv);
    } catch (const std::exception& ex) {
        std::cerr << "--sizes: " << ex.what() << "\n";
        return 1;
    }

    const auto max_size_bytes = zerokv::examples::message_kv_demo::max_size_bytes_for_sizes(sizes);

    std::vector<MemoryRegion::Ptr> workers_region(static_cast<size_t>(args.threads));
    std::vector<MessageKV::Ptr> workers_mq(static_cast<size_t>(args.threads));
    for (int i = 0; i < args.threads; ++i) {
        workers_mq[static_cast<size_t>(i)] = MessageKV::create(cfg);
        workers_mq[static_cast<size_t>(i)]->start(NodeConfig{
            .server_addr = args.server_addr,
            .local_data_addr = args.data_addr,
            .node_id = make_sender_node_id(args.node_id, 0, i),
        });
        workers_region[static_cast<size_t>(i)] =
            workers_mq[static_cast<size_t>(i)]->allocate_send_region(max_size_bytes);
    }

    auto run_round = [&](size_t protocol_round_index,
                         size_t report_round_index,
                         size_t size_bytes,
                         bool print_summary) -> bool {
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(args.threads));
        std::vector<std::exception_ptr> errors(static_cast<size_t>(args.threads));
        std::vector<uint64_t> send_us(static_cast<size_t>(args.threads), 0);
        const size_t total_bytes = size_bytes * static_cast<size_t>(args.threads);

        const auto round_start = SteadyClock::now();
        for (int i = 0; i < args.threads; ++i) {
            workers.emplace_back([&, i, protocol_round_index, size_bytes] {
                try {
                    auto& mq = workers_mq[static_cast<size_t>(i)];
                    auto& region = workers_region[static_cast<size_t>(i)];
                    const auto key = zerokv::examples::message_kv_demo::make_round_key(
                        protocol_round_index, size_bytes, static_cast<size_t>(i));
                    const auto payload = zerokv::examples::message_kv_demo::make_payload(
                        protocol_round_index, size_bytes, static_cast<size_t>(i));
                    std::memcpy(region->address(), payload.data(), size_bytes);
                    const auto send_start = SteadyClock::now();
                    mq->send_region(key, region, size_bytes);
                    const auto send_end = SteadyClock::now();
                    send_us[static_cast<size_t>(i)] = elapsed_us(send_start, send_end);
                    if (print_summary) {
                        std::cout << "SEND_OK key=" << key
                                  << " bytes=" << size_bytes
                                  << " send_us=" << send_us[static_cast<size_t>(i)]
                                  << "\n";
                    }
                } catch (...) {
                    errors[static_cast<size_t>(i)] = std::current_exception();
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }

        const auto round_end = SteadyClock::now();

        for (const auto& error : errors) {
            if (error) {
                try {
                    std::rethrow_exception(error);
                } catch (const std::exception& ex) {
                    std::cerr << "SEND_ERR message=" << ex.what() << "\n";
                }
                return false;
            }
        }

        uint64_t max_thread_send_us = 0;
        for (uint64_t value : send_us) {
            if (value > max_thread_send_us) {
                max_thread_send_us = value;
            }
        }
        const auto total_us = elapsed_us(round_start, round_end);
        if (print_summary) {
            std::cout << zerokv::examples::message_kv_demo::render_send_round_summary(
                             report_round_index,
                             size_bytes,
                             static_cast<size_t>(args.threads),
                             total_us,
                             max_thread_send_us,
                             total_bytes)
                      << "\n";
        }
        return true;
    };

    size_t protocol_round_index = 0;
    for (int warmup = 0; warmup < args.warmup_rounds; ++warmup, ++protocol_round_index) {
        if (!run_round(protocol_round_index, 0, sizes.front(), false)) {
            for (auto& mq : workers_mq) {
                mq->stop();
            }
            return 1;
        }
    }

    for (size_t round_index = 0; round_index < sizes.size(); ++round_index, ++protocol_round_index) {
        if (!run_round(protocol_round_index, round_index, sizes[round_index], true)) {
            for (auto& mq : workers_mq) {
                mq->stop();
            }
            return 1;
        }
    }

    for (auto& mq : workers_mq) {
        mq->stop();
    }

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, &args) || args.role.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    const auto cfg = Config::builder()
                         .set_transport(args.transport)
                         .set_connect_timeout(std::chrono::milliseconds(args.timeout_ms))
                         .build();

    if (args.role == "rank0") {
        return run_rank0(args, cfg);
    }
    if (args.role == "rank1") {
        return run_rank1(args, cfg);
    }

    print_usage(argv[0]);
    return 1;
}

#endif  // MESSAGE_KV_DEMO_BUILD_TESTS
