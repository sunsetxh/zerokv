#include <zerokv/config.h>
#include <zerokv/kv.h>
#include <zerokv/message_kv.h>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace zerokv;
using namespace zerokv::kv;

namespace {

struct Args {
    std::string role;
    std::string listen_addr;
    std::string server_addr;
    std::string data_addr;
    std::string node_id = "rank0";
    std::string transport = "tcp";
    int threads = 4;
    int timeout_ms = 5000;
    int cleanup_wait_ms = 1500;
    int post_recv_wait_ms = 2000;
};

void print_usage(const char* argv0) {
    std::cerr
        << "Usage:\n"
        << "  " << argv0
        << " --role rank0 --listen <addr> --data-addr <addr> --node-id <id>\n"
        << "           [--threads 4] [--timeout-ms 5000] [--post-recv-wait-ms 2000]"
        << " [--transport tcp|rdma]\n"
        << "  " << argv0
        << " --role rank1 --server-addr <addr> --data-addr <addr> --node-id <id>\n"
        << "           [--threads 4] [--cleanup-wait-ms 1500] [--transport tcp|rdma]\n";
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
        } else if (arg == "--threads" && i + 1 < argc) {
            args->threads = std::stoi(argv[++i]);
        } else if (arg == "--timeout-ms" && i + 1 < argc) {
            args->timeout_ms = std::stoi(argv[++i]);
        } else if (arg == "--cleanup-wait-ms" && i + 1 < argc) {
            args->cleanup_wait_ms = std::stoi(argv[++i]);
        } else if (arg == "--post-recv-wait-ms" && i + 1 < argc) {
            args->post_recv_wait_ms = std::stoi(argv[++i]);
        } else {
            return false;
        }
    }
    return true;
}

std::string make_key(int sender_rank, int receiver_rank, int thread_index) {
    return "msg-rank" + std::to_string(sender_rank) + "-to-rank" +
           std::to_string(receiver_rank) + "-thread" + std::to_string(thread_index);
}

std::string make_payload(int sender_rank, int thread_index) {
    return "payload-from-rank" + std::to_string(sender_rank) + "-thread" +
           std::to_string(thread_index);
}

std::string make_sender_node_id(const std::string& base, int thread_index) {
    return base + "-thread" + std::to_string(thread_index);
}

int run_rank0(const Args& args, const Config& cfg) {
    if (args.listen_addr.empty() || args.data_addr.empty() || args.node_id.empty()) {
        std::cerr << "--listen, --data-addr, and --node-id are required for rank0\n";
        return 1;
    }
    if (args.threads <= 0) {
        std::cerr << "--threads must be > 0\n";
        return 1;
    }
    if (args.post_recv_wait_ms < 0) {
        std::cerr << "--post-recv-wait-ms must be >= 0\n";
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

    auto ctx = Context::create(cfg);
    if (!ctx) {
        std::cerr << "failed to create Context\n";
        mq->stop();
        server->stop();
        return 1;
    }

    std::vector<MessageKV::BatchRecvItem> items;
    std::vector<std::string> keys;
    std::vector<std::string> payloads;
    std::vector<size_t> offsets;
    size_t total_bytes = 0;
    items.reserve(static_cast<size_t>(args.threads));
    keys.reserve(static_cast<size_t>(args.threads));
    payloads.reserve(static_cast<size_t>(args.threads));
    offsets.reserve(static_cast<size_t>(args.threads));

    for (int i = 0; i < args.threads; ++i) {
        const auto key = make_key(1, 0, i);
        const auto payload = make_payload(1, i);
        keys.push_back(key);
        payloads.push_back(payload);
        offsets.push_back(total_bytes);
        items.push_back(MessageKV::BatchRecvItem{
            .key = key,
            .length = payload.size(),
            .offset = total_bytes,
        });
        total_bytes += payload.size();
    }

    auto region = MemoryRegion::allocate(ctx, total_bytes);
    if (!region) {
        std::cerr << "failed to allocate receive region\n";
        mq->stop();
        return 1;
    }

    try {
        const auto result =
            mq->recv_batch(items, region, std::chrono::milliseconds(args.timeout_ms));

        std::cout << "RECV_BATCH completed=" << result.completed.size()
                  << " failed=" << result.failed.size()
                  << " timed_out=" << result.timed_out.size()
                  << " completed_all=" << (result.completed_all ? 1 : 0) << "\n";

        if (!result.completed_all || !result.failed.empty() || !result.timed_out.empty()) {
            mq->stop();
            server->stop();
            return 1;
        }

        for (int i = 0; i < args.threads; ++i) {
            const std::string value(
                static_cast<const char*>(region->address()) + offsets[static_cast<size_t>(i)],
                payloads[static_cast<size_t>(i)].size());
            std::cout << "RECV_OK key=" << keys[static_cast<size_t>(i)]
                      << " value=" << value << "\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(args.post_recv_wait_ms));
    } catch (const std::exception& ex) {
        std::cerr << "RECV_ERR message=" << ex.what() << "\n";
        mq->stop();
        server->stop();
        return 1;
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
    if (args.cleanup_wait_ms < 0) {
        std::cerr << "--cleanup-wait-ms must be >= 0\n";
        return 1;
    }

    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(args.threads));
    std::vector<std::exception_ptr> errors(static_cast<size_t>(args.threads));

    for (int i = 0; i < args.threads; ++i) {
        workers.emplace_back([&, i] {
            try {
                auto mq = MessageKV::create(cfg);
                mq->start(NodeConfig{
                    .server_addr = args.server_addr,
                    .local_data_addr = args.data_addr,
                    .node_id = make_sender_node_id(args.node_id, i),
                });

                const auto key = make_key(1, 0, i);
                const auto payload = make_payload(1, i);
                mq->send(key, payload.data(), payload.size());
                std::cout << "SEND_OK key=" << key << " value=" << payload << "\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(args.cleanup_wait_ms));
                mq->stop();
            } catch (...) {
                errors[static_cast<size_t>(i)] = std::current_exception();
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    for (const auto& error : errors) {
        if (error) {
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& ex) {
                std::cerr << "SEND_ERR message=" << ex.what() << "\n";
            }
            return 1;
        }
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

    const auto cfg = Config::builder().set_transport(args.transport).build();

    if (args.role == "rank0") {
        return run_rank0(args, cfg);
    }
    if (args.role == "rank1") {
        return run_rank1(args, cfg);
    }

    print_usage(argv[0]);
    return 1;
}
