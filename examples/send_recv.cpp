/// @file send_recv.cpp
/// @brief Send/recv example using tag-matched messaging.
///
/// Usage:
///   This example demonstrates the tag-matching API.
///   Full two-process test requires bootstrap mechanism.

#include <zerokv/config.h>
#include <zerokv/transport/worker.h>
#include <zerokv/transport/endpoint.h>

#include <iostream>
#include <vector>
#include <cstring>
#include <thread>

using namespace zerokv;
using namespace zerokv::transport;

int main() {
    // Create config - use TCP or shmem
    auto config = Config::builder()
        .set_transport("tcp")
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

    // Prepare message buffer
    std::vector<char> send_buffer(4096, 'A');
    std::vector<char> recv_buffer(4096, 0);

    Tag tag = make_tag(1, 42);

    // Example: Post a receive (worker-level)
    std::cout << "Posting tag_recv for tag=" << tag_user(tag) << "...\n";
    auto recv_future = worker->tag_recv(recv_buffer.data(), recv_buffer.size(), tag);

    if (!recv_future.request()) {
        std::cerr << "Failed to post receive\n";
        return 1;
    }

    std::cout << "Receive posted. Progressing worker...\n";

    // Progress until complete
    int progress_count = 0;
    while (!recv_future.ready() && progress_count < 100) {
        worker->progress();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        progress_count++;
    }

    if (recv_future.ready()) {
        auto [bytes, matched_tag] = recv_future.get();
        std::cout << "Received " << bytes << " bytes, tag=" << matched_tag << "\n";
    } else {
        std::cout << "No data received (timeout)\n";
    }

    std::cout << "Send/recv API example complete.\n";
    std::cout << "Note: Full two-process test requires bootstrap mechanism.\n";

    return 0;
}
