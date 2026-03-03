/**
 * ZeroKV Embedded Client Example
 *
 * This example demonstrates how to use ZeroKV client directly in your application.
 *
 * Compile (from project root):
 *   g++ -std=c++17 -O2 -I./include -L./build -lzerokv -lucp -lucs -luct -lpthread \
 *       examples/embedded_client.cc -o embedded_client
 *
 * Run:
 *   ./embedded_client
 *
 * Note: This example requires a running ZeroKV server.
 * Start the server first: ./zerokv_server
 */

#include <iostream>
#include <string>
#include <cstring>
#include "zerokv/client.h"
#include "zerokv/storage.h"

using namespace zerokv;

int main() {
    std::cout << "=== ZeroKV Embedded Client Example ===" << std::endl;

    // Create client
    Client client;

    // Connect to server
    Status status = client.connect({"127.0.0.1:5000"});
    if (status != Status::OK) {
        std::cerr << "Failed to connect to server: " << static_cast<int>(status) << std::endl;
        std::cerr << "Make sure the server is running first!" << std::endl;
        return 1;
    }

    std::cout << "Connected to server" << std::endl;

    // Put a key-value pair
    std::string key = "user:1001";
    std::string value = R"({"name": "Alice", "age": 30, "city": "Beijing"})";

    status = client.put(key, value);
    if (status != Status::OK) {
        std::cerr << "Failed to put value: " << static_cast<int>(status) << std::endl;
        return 1;
    }
    std::cout << "Put: key=" << key << ", value=" << value << std::endl;

    // Get the value
    std::string result;
    status = client.get(key, &result);

    if (status != Status::OK) {
        std::cerr << "Failed to get value: " << static_cast<int>(status) << std::endl;
        return 1;
    }

    std::cout << "Get: key=" << key << ", value=" << result << std::endl;

    // Verify value matches
    if (result == value) {
        std::cout << "Value verification: PASSED" << std::endl;
    } else {
        std::cout << "Value verification: FAILED (expected: " << value
                  << ", got: " << result << ")" << std::endl;
    }

    // Delete the key
    status = client.remove(key);
    if (status != Status::OK) {
        std::cerr << "Failed to remove key: " << static_cast<int>(status) << std::endl;
    } else {
        std::cout << "Removed key: " << key << std::endl;
    }

    // Disconnect
    client.disconnect();
    std::cout << "Disconnected from server" << std::endl;

    std::cout << "\n=== Embedded Client Example Completed ===" << std::endl;
    return 0;
}
