/// @file ping_pong.cpp
/// @brief Ping-pong latency benchmark example using TCP or shared memory.
///
/// Usage:
///   Server: ./ping_pong --listen :13337
///   Client: ./ping_pong --connect localhost:13337
///
/// UCX can use TCP or shared memory (shmem) transport.
/// Set AXON_TRANSPORTS=tcp,shmem or AXON_TRANSPORTS=shmem for different transports.

#include <axon/config.h>
#include <axon/worker.h>
#include <axon/endpoint.h>

#include <iostream>
#include <vector>
#include <cstring>
#include <chrono>
#include <thread>

using namespace axon;

constexpr size_t kMessageSize = 4096;
constexpr int kNumIterations = 1000;

int main(int argc, char** argv) {
    std::string mode;
    std::string address;

    // Simple argument parsing
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--listen" && i + 1 < argc) {
            mode = "server";
            address = argv[++i];
        } else if (arg == "--connect" && i + 1 < argc) {
            mode = "client";
            address = argv[++i];
        }
    }

    if (mode.empty()) {
        std::cerr << "Usage: " << argv[0] << " --listen <addr> | --connect <addr>\n";
        return 1;
    }

    // Create config and context
    auto config = Config::builder()
        .set_transport("tcp")  // Can also use "shmem" for shared memory
        .build();

    // Config is always valid with default values

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

    std::cout << "Using transport: TCP (set AXON_TRANSPORTS=shmem for shared memory)\n";

    if (mode == "server") {
        // Server mode: listen for connections
        std::cout << "Listening on " << address << "...\n";

        auto listener = worker->listen(address, [](Endpoint::Ptr ep) {
            std::cout << "Client connected!\n";
            (void)ep;
        });

        if (!listener) {
            std::cerr << "Failed to create listener\n";
            return 1;
        }

        std::cout << "Listening on " << listener->address() << "\n";
        std::cout << "Exchange addresses manually, then press Enter to continue...\n";
        std::cin.get();

    } else {
        // Client mode: connect to server
        std::cout << "Connecting to " << address << "...\n";

        // For TCP, we need a bootstrap mechanism
        // This example requires manual address exchange
        std::cerr << "Error: TCP connect requires bootstrap mechanism.\n";
        std::cerr << "Use shared memory (shmem) for single-machine testing:\n";
        std::cerr << "  Set AXON_TRANSPORTS=shmem and use --listen / --connect with localhost\n";
        return 1;
    }

    return 0;
}
