/**
 * ZeroKV CMake Example - Using ZeroKV as a library
 *
 * This example demonstrates how to use ZeroKV in your CMake project.
 *
 * Build:
 *   mkdir build
 *   cd build
 *   cmake ..
 *   make
 *
 * Run:
 *   ./my_app
 *
 * Note: This example requires ZeroKV to be built and installed.
 * Adjust the zerokv_DIR in CMakeLists.txt to point to your ZeroKV build directory.
 */

#include <iostream>
#include <memory>
#include "zerokv/storage.h"

int main() {
    std::cout << "=== ZeroKV CMake Example ===" << std::endl;

    // Create a storage engine with 1MB memory
    auto storage = std::make_unique<zerokv::StorageEngine>(1024 * 1024);

    // Put a key-value pair
    std::string key = "test_key";
    std::string value = "Hello from ZeroKV!";

    zerokv::Status status = storage->put(key, value.data(), value.size());
    if (status != zerokv::Status::OK) {
        std::cerr << "Failed to put value" << std::endl;
        return 1;
    }

    // Get the value
    char buffer[1024];
    size_t size = sizeof(buffer);
    status = storage->get(key, buffer, &size);

    if (status == zerokv::Status::OK) {
        buffer[size] = '\0';
        std::cout << "Got value: " << buffer << std::endl;
    } else {
        std::cerr << "Failed to get value" << std::endl;
        return 1;
    }

    std::cout << "CMake integration example completed successfully!" << std::endl;
    return 0;
}
