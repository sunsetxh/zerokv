/**
 * ZeroKV Embedded Server Example
 *
 * This example demonstrates how to embed ZeroKV server directly in your application.
 *
 * Compile (from project root):
 *   g++ -std=c++17 -O2 -I./include -L./build -lzerokv -lucp -lucs -luct -lpthread \
 *       examples/embedded_server.cc -o embedded_server
 *
 * Run:
 *   ./embedded_server
 */

#include <iostream>
#include <thread>
#include <chrono>
#include <cassert>
#include "zerokv/server.h"
#include "zerokv/storage.h"
#include "zerokv/client.h"

using namespace zerokv;

// Simple embedded server test
int main() {
    std::cout << "=== ZeroKV Embedded Server Example ===" << std::endl;

    // Create and start embedded server
    Server server;
    Status status = server.start("127.0.0.1", 5000, 64 * 1024 * 1024); // 64MB

    if (status != Status::OK) {
        std::cerr << "Failed to start server: " << static_cast<int>(status) << std::endl;
        return 1;
    }

    std::cout << "Server started on 127.0.0.1:5000" << std::endl;
    std::cout << "Server is running: " << (server.is_running() ? "yes" : "no") << std::endl;

    // In a real application, you would connect with a client here
    // For this example, we just demonstrate the embedded server lifecycle

    // Wait a bit
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Stop server
    server.stop();
    std::cout << "Server stopped" << std::endl;

    std::cout << "\n=== Embedded Server Example Completed ===" << std::endl;
    return 0;
}
