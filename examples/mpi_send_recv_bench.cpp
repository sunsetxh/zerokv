#include <mpi.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace zerokv::examples::mpi_send_recv_bench {

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
                                 uint64_t elapsed_us) {
    std::ostringstream out;
    out << "MPI_SEND_RECV_ROUND"
        << " role=" << role
        << " round=" << round
        << " size=" << size
        << " iters=" << iters
        << " total_bytes=" << total_bytes
        << " elapsed_us=" << elapsed_us
        << " throughput_MiBps="
        << throughput_mib_per_sec(total_bytes, std::chrono::microseconds(elapsed_us));
    return out.str();
}

}  // namespace zerokv::examples::mpi_send_recv_bench

#ifndef MPI_SEND_RECV_BENCH_BUILD_TESTS

namespace {

struct Args {
    std::string sizes_csv = "256K,512K,1M,2M,4M,8M,16M,32M,64M";
    int iters = 100;
    int warmup = 5;
};

void PrintUsage(const char* argv0) {
    std::cerr
        << "Usage:\n"
        << "  mpirun -n 2 " << argv0
        << " [--sizes 256K,512K,1M,2M,4M,8M,16M,32M,64M]"
           " [--iters 100] [--warmup 5]\n";
}

bool ParseArgs(int argc, char** argv, Args* args) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--sizes" || arg == "--size") && i + 1 < argc) {
            args->sizes_csv = argv[++i];
        } else if (arg == "--iters" && i + 1 < argc) {
            args->iters = std::atoi(argv[++i]);
        } else if (arg == "--warmup" && i + 1 < argc) {
            args->warmup = std::atoi(argv[++i]);
        } else {
            return false;
        }
    }
    return args->iters > 0 && args->warmup >= 0;
}

}  // namespace

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int world_rank = 0;
    int world_size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    Args args;
    if (!ParseArgs(argc, argv, &args)) {
        if (world_rank == 0) {
            PrintUsage(argv[0]);
        }
        MPI_Finalize();
        return 1;
    }

    if (world_size != 2) {
        if (world_rank == 0) {
            std::cerr << "mpi_send_recv_bench requires exactly 2 MPI ranks." << std::endl;
        }
        MPI_Finalize();
        return 1;
    }

    std::vector<size_t> sizes;
    try {
        sizes = zerokv::examples::mpi_send_recv_bench::parse_sizes_csv(args.sizes_csv);
    } catch (const std::exception& ex) {
        if (world_rank == 0) {
            std::cerr << ex.what() << std::endl;
        }
        MPI_Finalize();
        return 1;
    }

    const size_t max_size = zerokv::examples::mpi_send_recv_bench::max_size_bytes_for_sizes(sizes);
    std::vector<char> buffer(max_size, world_rank == 0 ? 0 : 'x');
    MPI_Status status{};

    for (size_t round_index = 0; round_index < sizes.size(); ++round_index) {
        const size_t size = sizes[round_index];
        const int count = static_cast<int>(size);

        MPI_Barrier(MPI_COMM_WORLD);
        for (int i = 0; i < args.warmup; ++i) {
            if (world_rank == 0) {
                MPI_Recv(buffer.data(), count, MPI_BYTE, 1, static_cast<int>(round_index),
                         MPI_COMM_WORLD, &status);
            } else {
                MPI_Send(buffer.data(), count, MPI_BYTE, 0, static_cast<int>(round_index),
                         MPI_COMM_WORLD);
            }
        }

        MPI_Barrier(MPI_COMM_WORLD);
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < args.iters; ++i) {
            if (world_rank == 0) {
                MPI_Recv(buffer.data(), count, MPI_BYTE, 1, static_cast<int>(round_index),
                         MPI_COMM_WORLD, &status);
            } else {
                MPI_Send(buffer.data(), count, MPI_BYTE, 0, static_cast<int>(round_index),
                         MPI_COMM_WORLD);
            }
        }
        MPI_Barrier(MPI_COMM_WORLD);
        const auto end = std::chrono::steady_clock::now();

        const size_t total_bytes = size * static_cast<size_t>(args.iters);
        const auto elapsed_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
        const char* role = world_rank == 0 ? "server" : "client";
        std::cout << zerokv::examples::mpi_send_recv_bench::render_round_summary(
                         role, round_index, size, args.iters, total_bytes, elapsed_us)
                  << std::endl;
    }

    MPI_Finalize();
    return 0;
}

#endif
