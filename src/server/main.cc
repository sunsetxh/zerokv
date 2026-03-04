#include <iostream>
#include <csignal>
#include <cstdlib>
#include <thread>
#include "zerokv/server.h"
#include "zerokv/storage.h"
#include "zerokv/protocol.h"
#include "zerokv/transport.h"

#ifdef USE_UCX
#include "transport/ucx_transport.h"
#endif

using namespace zerokv;

static std::atomic<bool> g_running{true};

void signal_handler(int signal) {
    std::cout << "\nShutting down..." << std::endl;
    g_running = false;
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -a <addr>   Listen address (default: 0.0.0.0)\n"
              << "  -p <port>   Listen port (default: 5000)\n"
              << "  -m <size>   Max memory in MB (default: 1024)\n"
              << "  -h          Show this help\n";
}

int main(int argc, char** argv) {
    std::string listen_addr = "0.0.0.0";
    uint16_t port = 5000;
    size_t max_memory = 1024 * 1024 * 1024; // 1GB

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-a" && i + 1 < argc) {
            listen_addr = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "-m" && i + 1 < argc) {
            max_memory = std::stoi(argv[++i]) * 1024 * 1024;
        } else if (arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
    }

    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "=== ZeroKV Server ===" << std::endl;
    std::cout << "Listen: " << listen_addr << ":" << port << std::endl;
    std::cout << "Max Memory: " << (max_memory / 1024 / 1024) << " MB" << std::endl;

    // Create storage
    auto storage = std::make_unique<StorageEngine>(max_memory);

    // Create transport
#ifdef USE_UCX
    auto transport = std::make_unique<UCXTransport>();
    Status status = transport->initialize();
    if (status != Status::OK) {
        std::cerr << "Failed to initialize transport" << std::endl;
        return 1;
    }

    // Pass storage to transport for request handling
    transport->set_storage(std::shared_ptr<StorageEngine>(storage.release()));

    status = transport->listen(listen_addr, port);
    if (status != Status::OK) {
        std::cerr << "Failed to listen on " << listen_addr << ":" << port << std::endl;
        return 1;
    }
#else
    std::cout << "Warning: UCX not available, running in storage-only mode" << std::endl;
#endif

    std::cout << "Server started successfully" << std::endl;

    // Main loop
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "Server stopped" << std::endl;
    return 0;
}
