/// @file examples/cpp_usage.cpp
/// @brief Demonstrates how to use the AXON library for point-to-point communication.
///
/// Build:  g++ -std=c++20 -I../include cpp_usage.cpp -laxon -lpthread -o demo

#include "axon/axon.h"

#include <iostream>
#include <thread>
#include <vector>

// ============================================================================
// Example 1: Basic tag-matched send/recv (two-sided)
// ============================================================================
void example_tag_messaging() {
    // --- Configuration ---
    auto cfg = axon::Config::builder()
                   .set_transport("ucx")
                   .set_num_workers(2)
                   .set_memory_pool_size(128 * 1024 * 1024)  // 128 MiB
                   .enable_registration_cache()
                   .set("UCX_TLS", "rc,ud,shm,self")
                   .from_env()  // override with AXON_* env vars
                   .build();

    auto ctx = axon::Context::create(cfg);

    // --- Sender side ---
    auto sender_worker = axon::Worker::create(ctx, /*index=*/0);
    auto send_ep_future = sender_worker->connect("192.168.1.2:13337");

    // Drive progress while connecting
    sender_worker->run_until([&] { return send_ep_future.ready(); });
    auto send_ep = send_ep_future.get();

    // Prepare data
    std::vector<float> data(1024, 3.14f);
    axon::Tag tag = axon::make_tag(/*context_id=*/0, /*user_tag=*/42);

    // --- Async send (small message, eager path) ---
    auto send_future = send_ep->tag_send(data.data(), data.size() * sizeof(float), tag);

    // ... do other work while send is in progress ...

    // Wait for completion
    sender_worker->run_until([&] { return send_future.ready(); });
    send_future.get();  // throws on error

    std::cout << "Send complete!\n";
}

// ============================================================================
// Example 2: Zero-copy large transfer with pre-registered memory
// ============================================================================
void example_zero_copy_large() {
    auto ctx = axon::Context::create();
    auto worker = axon::Worker::create(ctx);

    // Pre-allocate and register a 1 GB buffer
    constexpr size_t kSize = 1ULL << 30;  // 1 GiB
    auto region = axon::MemoryRegion::allocate(ctx, kSize, axon::MemoryType::kHost);

    // Fill data (cast to typed span)
    auto span = region->as_span<double>();
    for (size_t i = 0; i < span.size(); ++i) {
        span[i] = static_cast<double>(i);
    }

    // Connect and send
    auto ep_future = worker->connect("10.0.0.1:13337");
    worker->run_until([&] { return ep_future.ready(); });
    auto ep = ep_future.get();

    axon::Tag tag = axon::make_tag(0, 1);

    // Zero-copy send: the registered region is sent directly via RDMA rendezvous.
    // No copy into an intermediate buffer.
    auto send_future = ep->tag_send(region, tag);
    worker->run_until([&] { return send_future.ready(); });
    send_future.get();
}

// ============================================================================
// Example 3: One-sided RDMA put/get
// ============================================================================
void example_rdma() {
    auto ctx = axon::Context::create();
    auto worker = axon::Worker::create(ctx);

    // --- Local memory ---
    auto local_region = axon::MemoryRegion::allocate(ctx, 4096);

    // --- Exchange rkeys out-of-band (e.g. via tag_send/tag_recv) ---
    // Assume we received:  remote_addr (uint64_t) + remote_rkey (RemoteKey)
    uint64_t       remote_addr = 0;  // placeholder
    axon::RemoteKey remote_rkey;      // placeholder

    auto ep_future = worker->connect("10.0.0.1:13337");
    worker->run_until([&] { return ep_future.ready(); });
    auto ep = ep_future.get();

    // RDMA Write: local -> remote
    auto put_future = ep->put(local_region, /*local_offset=*/0,
                              remote_addr, remote_rkey, /*length=*/4096);

    // RDMA Read:  remote -> local
    auto get_future = ep->get(local_region, /*local_offset=*/0,
                              remote_addr, remote_rkey, /*length=*/4096);

    // Flush to ensure remote visibility
    auto flush_future = ep->flush();

    worker->run_until([&] { return flush_future.ready(); });
    flush_future.get();
}

// ============================================================================
// Example 4: Memory pool + callback-based completion
// ============================================================================
void example_memory_pool_and_callbacks() {
    auto ctx = axon::Context::create();
    auto worker = axon::Worker::create(ctx);

    // Create a pool: avoids repeated registration on the hot path
    auto pool = axon::MemoryPool::create(ctx, 64 * 1024 * 1024);

    auto ep_future = worker->connect("10.0.0.1:13337");
    worker->run_until([&] { return ep_future.ready(); });
    auto ep = ep_future.get();

    // Hot loop: allocate from pool, send, return to pool
    for (int i = 0; i < 10000; ++i) {
        auto buf = pool->allocate(4096);
        std::memset(buf.data, 0, 4096);

        axon::Tag tag = axon::make_tag(0, static_cast<uint32_t>(i));

        // Use callback-based completion (avoids polling overhead)
        auto future = ep->tag_send(buf.region, tag);
        future.on_complete([&pool, buf_copy = buf](axon::Status st, auto) mutable {
            if (!st.ok()) {
                std::cerr << "Send failed: " << st.message() << "\n";
            }
            // Return buffer to pool (captured by value)
            pool->deallocate(buf_copy);
        });
    }

    // Drain all pending operations
    worker->run_until([&] { return pool->used_bytes() == 0; });
}

// ============================================================================
// Example 5: Server (listener) with accept callback
// ============================================================================
void example_server() {
    auto ctx = axon::Context::create();
    auto worker = axon::Worker::create(ctx);

    auto listener = worker->listen(":13337", [&](axon::Endpoint::Ptr ep) {
        std::cout << "Accepted connection from " << ep->remote_address() << "\n";

        // Post a recv for each new connection
        std::vector<char> buf(1024 * 1024);
        axon::Tag tag = axon::make_tag(0, 0);

        auto recv_future = ep->tag_recv(buf.data(), buf.size(),
                                        tag, axon::kTagMaskUser);
        recv_future.on_complete([](axon::Status st, auto result) {
            auto [bytes, matched_tag] = result;
            std::cout << "Received " << bytes << " bytes, tag="
                      << axon::tag_user(matched_tag) << "\n";
        });
    });

    std::cout << "Listening on " << listener->address() << "\n";
    worker->run();  // blocks forever
}

// ============================================================================
// Example 6: Batch futures with wait_all
// ============================================================================
void example_batch() {
    auto ctx = axon::Context::create();
    auto worker = axon::Worker::create(ctx);

    auto ep_future = worker->connect("10.0.0.1:13337");
    worker->run_until([&] { return ep_future.ready(); });
    auto ep = ep_future.get();

    // Send many small messages in parallel
    std::vector<axon::Future<void>> futures;
    futures.reserve(100);

    for (int i = 0; i < 100; ++i) {
        float val = static_cast<float>(i);
        futures.push_back(ep->tag_send(&val, sizeof(val), axon::make_tag(0, i)));
    }

    // Wait for all sends to complete (with timeout)
    auto status = axon::wait_all(futures, std::chrono::seconds{5});
    status.throw_if_error();
}

// ============================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <example_number>\n";
        return 1;
    }

    switch (std::atoi(argv[1])) {
        case 1: example_tag_messaging(); break;
        case 2: example_zero_copy_large(); break;
        case 3: example_rdma(); break;
        case 4: example_memory_pool_and_callbacks(); break;
        case 5: example_server(); break;
        case 6: example_batch(); break;
        default: std::cerr << "Unknown example\n"; return 1;
    }
    return 0;
}
