#include "yr/alps_kv_api.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace zerokv::examples::alps_kv_bench {

struct RoundTimingSummary {
    uint64_t avg_control_request_grant_us = 0;
    uint64_t avg_put_us = 0;
    uint64_t avg_flush_us = 0;
    uint64_t avg_write_done_ack_us = 0;
};

std::vector<size_t> parse_sizes_csv(const std::string& csv) {
    std::vector<size_t> sizes;
    std::stringstream stream(csv);
    std::string token;

    while (std::getline(stream, token, ',')) {
        token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        }), token.end());
        if (token.empty()) {
            throw std::invalid_argument("empty size token");
        }

        size_t value_end = 0;
        size_t value = 0;
        try {
            value = std::stoull(token, &value_end);
        } catch (const std::exception&) {
            throw std::invalid_argument("invalid size token: " + token);
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
            throw std::invalid_argument("invalid size suffix: " + suffix);
        }

        sizes.push_back(value * scale);
    }

    if (sizes.empty()) {
        throw std::invalid_argument("size list is empty");
    }

    return sizes;
}

size_t max_size_bytes_for_sizes(const std::vector<size_t>& sizes) {
    return *std::max_element(sizes.begin(), sizes.end());
}

double throughput_mib_per_sec(size_t total_bytes, std::chrono::steady_clock::duration elapsed) {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    if (us <= 0) {
        return 0.0;
    }
    const double seconds = static_cast<double>(us) / 1000000.0;
    return static_cast<double>(total_bytes) / (1024.0 * 1024.0) / seconds;
}

std::string render_round_summary(const char* role,
                                 size_t round,
                                 size_t size,
                                 int iters,
                                 size_t total_bytes,
                                 uint64_t elapsed_us,
                                 const RoundTimingSummary* timing) {
    std::ostringstream out;
    out << "ALPS_KV_ROUND"
        << " role=" << role
        << " round=" << round
        << " size=" << size
        << " iters=" << iters
        << " total_bytes=" << total_bytes
        << " elapsed_us=" << elapsed_us
        << " throughput_MiBps="
        << throughput_mib_per_sec(total_bytes, std::chrono::microseconds(elapsed_us));
    if (timing != nullptr) {
        out << " avg_control_request_grant_us=" << timing->avg_control_request_grant_us
            << " avg_put_us=" << timing->avg_put_us
            << " avg_flush_us=" << timing->avg_flush_us
            << " avg_write_done_ack_us=" << timing->avg_write_done_ack_us;
    }
    return out.str();
}

std::string render_listen_address_line(const std::string& address) {
    return "ALPS_KV_LISTEN address=" + address;
}

}  // namespace zerokv::examples::alps_kv_bench

#ifndef ALPS_KV_BENCH_BUILD_TESTS

namespace {

struct Args {
    std::string mode;
    std::string host = "127.0.0.1";
    std::string sizes_csv = "256K,512K,1M,2M,4M,8M,16M,32M,64M";
    int port = 16000;
    int iters = 100;
    int warmup = 5;
    int timeout_ms = 5000;
    int threads = 1;  // number of concurrent sender threads (client) / messages per iter (server)
};

void PrintUsage(const char* argv0) {
    std::cerr
        << "Usage:\n"
        << "  " << argv0
        << " --mode server --port 16000"
           " [--sizes 256K,512K,1M,2M,4M,8M,16M,32M,64M]"
           " [--iters 100] [--warmup 5] [--threads 1]\n"
        << "  " << argv0
        << " --mode client --host 127.0.0.1 --port 16000"
           " [--sizes 256K,512K,1M,2M,4M,8M,16M,32M,64M]"
           " [--iters 100] [--warmup 5] [--threads 1]\n"
        << "\n"
        << "  --threads N  server: receive N messages per iter via ReadBytesBatch.\n"
        << "               client: spawn N threads each with an independent connection.\n";
}

bool ParseArgs(int argc, char** argv, Args* args) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            args->mode = argv[++i];
        } else if (arg == "--host" && i + 1 < argc) {
            args->host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            args->port = std::atoi(argv[++i]);
        } else if (arg == "--sizes" && i + 1 < argc) {
            args->sizes_csv = argv[++i];
        } else if (arg == "--size" && i + 1 < argc) {
            args->sizes_csv = argv[++i];
        } else if (arg == "--iters" && i + 1 < argc) {
            args->iters = std::atoi(argv[++i]);
        } else if (arg == "--warmup" && i + 1 < argc) {
            args->warmup = std::atoi(argv[++i]);
        } else if (arg == "--timeout-ms" && i + 1 < argc) {
            args->timeout_ms = std::atoi(argv[++i]);
        } else if (arg == "--threads" && i + 1 < argc) {
            args->threads = std::atoi(argv[++i]);
        } else {
            return false;
        }
    }

    return !args->mode.empty() && args->port > 0 && args->iters > 0 && args->warmup >= 0 &&
           args->threads >= 1;
}

int message_index(size_t round_index, int per_round_count, int iter_index) {
    return static_cast<int>(round_index * static_cast<size_t>(per_round_count) + static_cast<size_t>(iter_index));
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!ParseArgs(argc, argv, &args)) {
        PrintUsage(argv[0]);
        return 1;
    }

    std::vector<size_t> sizes;
    try {
        sizes = zerokv::examples::alps_kv_bench::parse_sizes_csv(args.sizes_csv);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }

    const size_t max_size = zerokv::examples::alps_kv_bench::max_size_bytes_for_sizes(sizes);
    const int per_round_count = args.warmup + args.iters;

    // -------------------------------------------------------------------------
    // Server (RANK0): single listen, receives args.threads messages per iter
    // via ReadBytesBatch (all in parallel).
    // -------------------------------------------------------------------------
    if (args.mode == "server") {
        if (!YR::SetClient("0.0.0.0", args.port, args.timeout_ms)) {
            std::cerr << "server failed to listen on port " << args.port << std::endl;
            return 1;
        }
        std::cout << zerokv::examples::alps_kv_bench::render_listen_address_line(
                         YR::GetLocalAddress())
                  << std::endl;

        // Allocate one buffer per thread slot.
        std::vector<std::vector<char>> bufs(static_cast<size_t>(args.threads),
                                            std::vector<char>(max_size, 0));

        for (size_t round_index = 0; round_index < sizes.size(); ++round_index) {
            const size_t size = sizes[round_index];

            auto recv_iter = [&](int global_iter, bool /*timing*/) {
                std::vector<void*>      ptrs(static_cast<size_t>(args.threads));
                std::vector<size_t>     szs(static_cast<size_t>(args.threads), size);
                std::vector<int>        tags(static_cast<size_t>(args.threads), 1);
                std::vector<int>        idxs(static_cast<size_t>(args.threads));
                std::vector<int>        srcs(static_cast<size_t>(args.threads));
                std::vector<int>        dsts(static_cast<size_t>(args.threads), 1);
                for (int t = 0; t < args.threads; ++t) {
                    ptrs[static_cast<size_t>(t)] = bufs[static_cast<size_t>(t)].data();
                    idxs[static_cast<size_t>(t)] = message_index(round_index, per_round_count,
                                                                  global_iter) * args.threads + t;
                    srcs[static_cast<size_t>(t)] = t;
                }
                YR::ReadBytesBatch(ptrs, szs, tags, idxs, srcs, dsts);
            };

            for (int i = 0; i < args.warmup; ++i) {
                recv_iter(i, false);
            }

            const auto begin = std::chrono::steady_clock::now();
            for (int i = 0; i < args.iters; ++i) {
                recv_iter(args.warmup + i, true);
            }
            const auto end = std::chrono::steady_clock::now();

            const size_t total_bytes =
                size * static_cast<size_t>(args.iters) * static_cast<size_t>(args.threads);
            const auto elapsed_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());

            std::cout << zerokv::examples::alps_kv_bench::render_round_summary(
                             "server", round_index, size, args.iters, total_bytes, elapsed_us,
                             nullptr)
                      << " threads=" << args.threads << std::endl;
        }

        YR::ShutdownClient();
        return 0;
    }

    // -------------------------------------------------------------------------
    // Client (RANK1): args.threads threads, each with its own connection.
    // All threads synchronize per iter so the wall-clock timing is fair.
    // -------------------------------------------------------------------------
    if (args.mode == "client") {
        if (!YR::SetClient(args.host.c_str(), args.port, args.timeout_ms)) {
            std::cerr << "client failed to connect to " << args.host << ":" << args.port
                      << std::endl;
            return 1;
        }

        // Per-thread send buffer.
        std::vector<std::vector<char>> bufs(static_cast<size_t>(args.threads),
                                            std::vector<char>(max_size, 'x'));

        // Spawn N threads to run one phase (warmup or timed) of a single round.
        // Returns false if any thread reported an error.
        auto run_phase = [&](size_t round_index, int phase_base, int phase_count) -> bool {
            const size_t size = sizes[round_index];
            std::atomic<bool> phase_error{false};
            std::vector<std::thread> workers;
            workers.reserve(static_cast<size_t>(args.threads));
            for (int t = 0; t < args.threads; ++t) {
                workers.emplace_back([&, t] {
                    void* buf = bufs[static_cast<size_t>(t)].data();
                    for (int i = 0; i < phase_count; ++i) {
                        const int idx =
                            message_index(round_index, per_round_count, phase_base + i) *
                                args.threads +
                            t;
                        if (!YR::WriteBytes(buf, size, 1, idx, t, 1)) {
                            phase_error.store(true);
                        }
                    }
                });
            }
            for (auto& w : workers) {
                w.join();
            }
            return !phase_error.load();
        };

        for (size_t round_index = 0; round_index < sizes.size(); ++round_index) {
            const size_t size = sizes[round_index];

            if (!run_phase(round_index, 0, args.warmup)) {
                std::cerr << "warmup failed at round " << round_index << std::endl;
                YR::ShutdownClient();
                return 1;
            }

            YR::ResetWriteTimingStats();
            const auto begin = std::chrono::steady_clock::now();
            if (!run_phase(round_index, args.warmup, args.iters)) {
                std::cerr << "send failed at round " << round_index << std::endl;
                YR::ShutdownClient();
                return 1;
            }
            const auto end = std::chrono::steady_clock::now();
            const auto timing = YR::GetWriteTimingStats();

            const size_t total_bytes =
                size * static_cast<size_t>(args.iters) * static_cast<size_t>(args.threads);
            const auto elapsed_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
            const uint64_t write_ops = timing.write_ops == 0 ? 1 : timing.write_ops;
            const zerokv::examples::alps_kv_bench::RoundTimingSummary round_timing{
                .avg_control_request_grant_us = timing.control_request_grant_us / write_ops,
                .avg_put_us = timing.rdma_put_us / write_ops,
                .avg_flush_us = timing.flush_us / write_ops,
                .avg_write_done_ack_us = timing.write_done_ack_us / write_ops,
            };
            std::cout << zerokv::examples::alps_kv_bench::render_round_summary(
                             "client", round_index, size, args.iters, total_bytes, elapsed_us,
                             &round_timing)
                      << " threads=" << args.threads << std::endl;
        }

        YR::ShutdownClient();
        return 0;
    }

    PrintUsage(argv[0]);
    return 1;
}

#endif
