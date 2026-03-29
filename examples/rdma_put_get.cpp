/// @file rdma_put_get.cpp
/// @brief RDMA put/get and atomic operations across two nodes.
///
/// Usage:
///   Server: ./rdma_put_get --listen 0.0.0.0:13337 [--transport rdma|tcp]
///   Client: ./rdma_put_get --connect 10.0.0.1:13337 [--transport rdma|tcp]
///
/// The server registers memory and sends its rkey to the client.
/// The client performs RDMA put, then RDMA get to verify, then atomic ops.

#include <axon/config.h>
#include <axon/worker.h>
#include <axon/endpoint.h>
#include <axon/memory.h>

#include <iostream>
#include <vector>
#include <cstring>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>

using namespace axon;

constexpr size_t kDefaultBufferSize = 4096;
constexpr Tag kRkeyTag = make_tag(1, 100);   // tag for rkey exchange
constexpr Tag kDataTag = make_tag(1, 101);   // tag for data notifications
constexpr Tag kSyncTag = make_tag(1, 102);   // tag for sync/ack

// Serialize RemoteKey into a buffer for tag_send
static std::vector<uint8_t> serialize_rkey(const RemoteKey& rkey, uint64_t remote_addr) {
    std::vector<uint8_t> buf(8 + rkey.size());
    uint64_t addr_be = remote_addr;
    std::memcpy(buf.data(), &addr_be, 8);
    std::memcpy(buf.data() + 8, rkey.bytes(), rkey.size());
    return buf;
}

// Deserialize
static bool deserialize_rkey(const void* data, size_t len,
                             RemoteKey& rkey, uint64_t& remote_addr) {
    if (len < 8) return false;
    std::memcpy(&remote_addr, data, 8);
    rkey.data.resize(len - 8);
    std::memcpy(rkey.data.data(), static_cast<const uint8_t*>(data) + 8, len - 8);
    return true;
}

// Global endpoint for server callback
static std::shared_ptr<Endpoint> g_server_ep;
static std::mutex g_ep_mutex;
static std::condition_variable g_ep_cv;

int main(int argc, char** argv) {
    std::string mode;
    std::string address;
    std::string transport = "tcp";
    size_t buffer_size = kDefaultBufferSize;
    size_t iterations = 1;
    bool skip_atomics = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--listen" && i + 1 < argc) {
            mode = "server";
            address = argv[++i];
        } else if (arg == "--connect" && i + 1 < argc) {
            mode = "client";
            address = argv[++i];
        } else if (arg == "--transport" && i + 1 < argc) {
            transport = argv[++i];
        } else if (arg == "--size" && i + 1 < argc) {
            buffer_size = static_cast<size_t>(std::stoull(argv[++i]));
        } else if (arg == "--iters" && i + 1 < argc) {
            iterations = static_cast<size_t>(std::stoull(argv[++i]));
        } else if (arg == "--skip-atomics") {
            skip_atomics = true;
        }
    }

    if (mode.empty()) {
        std::cerr << "Usage: " << argv[0]
                  << " --listen <addr> | --connect <addr> [--transport rdma|tcp]"
                  << " [--size bytes] [--iters n] [--skip-atomics]\n";
        return 1;
    }

    auto config = Config::builder()
        .set_transport(transport)
        .build();

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

    std::cout << "Transport: " << transport << "\n";

    if (mode == "server") {
        // ================================================================
        // Server: listen → accept → register memory → send rkey → wait
        // ================================================================
        std::cout << "Listening on " << address << "...\n";

        auto listener = worker->listen(address, [](Endpoint::Ptr ep) {
            std::lock_guard<std::mutex> lock(g_ep_mutex);
            g_server_ep = ep;
            g_ep_cv.notify_one();
            std::cout << "Client connected!\n";
        });

        if (!listener) {
            std::cerr << "Failed to create listener\n";
            return 1;
        }

        std::cout << "Listening on " << listener->address() << "\n";
        worker->start_progress_thread();

        // Wait for connection
        {
            std::unique_lock<std::mutex> lock(g_ep_mutex);
            g_ep_cv.wait(lock, [] { return g_server_ep != nullptr; });
        }

        auto ep = g_server_ep;

        // Register memory region for RDMA
        auto region = MemoryRegion::allocate(ctx, buffer_size);
        if (!region) {
            std::cerr << "Failed to allocate memory region\n";
            return 1;
        }

        // Initialize memory with known pattern
        auto* buf = static_cast<uint8_t*>(region->address());
        std::memset(buf, 0, buffer_size);

        // Pack and send rkey to client
        auto rkey = region->remote_key();
        if (rkey.empty()) {
            std::cerr << "Failed to pack remote key\n";
            return 1;
        }

        auto rkey_buf = serialize_rkey(rkey, reinterpret_cast<uint64_t>(region->address()));
        std::cout << "Sending rkey (" << rkey.size() << " bytes) to client...\n";

        auto send_fut = ep->tag_send(rkey_buf.data(), rkey_buf.size(), kRkeyTag);
        send_fut.get();

        // Wait for client to finish RDMA operations
        // Receive "done" notification
        uint8_t sync_buf[1] = {};
        auto recv_fut = ep->tag_recv(sync_buf, sizeof(sync_buf), kSyncTag);
        recv_fut.get();

        // Verify data written by client via RDMA put
        std::cout << "\nVerifying data written by client via RDMA put:\n";
        bool data_ok = true;
        for (size_t i = 0; i < buffer_size; ++i) {
            uint8_t expected = static_cast<uint8_t>(i & 0xFF);
            if (buf[i] != expected) {
                std::cout << "  Mismatch at offset " << i
                          << ": got " << static_cast<int>(buf[i])
                          << ", expected " << static_cast<int>(expected) << "\n";
                data_ok = false;
                if (i > 10) break;  // Only show first few errors
            }
        }
        if (data_ok) {
            std::cout << "  RDMA put data verified OK (" << buffer_size << " bytes)\n";
        }

        // Verify atomic result
        uint64_t atomic_val = 0;
        std::memcpy(&atomic_val, buf, sizeof(uint64_t));
        std::cout << "  Atomic counter value: " << atomic_val << "\n";

        worker->stop_progress_thread();
        std::cout << "\nServer done.\n";

    } else {
        // ================================================================
        // Client: connect → recv rkey → RDMA put → RDMA get → atomics
        // ================================================================
        std::cout << "Connecting to " << address << "...\n";

        auto connect_fut = worker->connect(address);
        auto ep = connect_fut.get();
        if (!ep) {
            std::cerr << "Failed to connect\n";
            return 1;
        }
        std::cout << "Connected!\n";

        worker->start_progress_thread();

        // Receive server's rkey
        std::vector<uint8_t> rkey_recv_buf(4096);
        auto recv_fut = ep->tag_recv(rkey_recv_buf.data(), rkey_recv_buf.size(), kRkeyTag);
        auto [rkey_bytes, _] = recv_fut.get();
        rkey_recv_buf.resize(rkey_bytes);

        RemoteKey server_rkey;
        uint64_t server_remote_addr = 0;
        if (!deserialize_rkey(rkey_recv_buf.data(), rkey_recv_buf.size(),
                              server_rkey, server_remote_addr)) {
            std::cerr << "Failed to deserialize rkey\n";
            return 1;
        }
        std::cout << "Received server rkey (" << server_rkey.size() << " bytes)\n";

        // Allocate local memory
        auto local_region = MemoryRegion::allocate(ctx, buffer_size);
        if (!local_region) {
            std::cerr << "Failed to allocate local memory\n";
            return 1;
        }

        // ---- RDMA Put ----
        std::cout << "\n--- RDMA Put ---\n";
        auto* local_buf = static_cast<uint8_t*>(local_region->address());
        for (size_t i = 0; i < buffer_size; ++i) {
            local_buf[i] = static_cast<uint8_t>(i & 0xFF);
        }

        auto start = std::chrono::high_resolution_clock::now();
        for (size_t iter = 0; iter < iterations; ++iter) {
            auto put_fut = ep->put(local_region, 0, server_remote_addr, server_rkey, buffer_size);
            put_fut.get();
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto put_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        auto put_total_bytes = static_cast<double>(buffer_size) * static_cast<double>(iterations);
        auto put_mbps = (put_us > 0) ? (put_total_bytes / static_cast<double>(put_us)) : 0.0;
        auto put_avg_us = static_cast<double>(put_us) / static_cast<double>(iterations);
        std::cout << "  Put " << buffer_size << " bytes x " << iterations
                  << " in " << put_us << " us"
                  << " (avg " << put_avg_us << " us/op, "
                  << put_mbps << " MB/s)\n";

        // ---- RDMA Get (verify) ----
        std::cout << "\n--- RDMA Get (verify) ---\n";
        std::memset(local_buf, 0, buffer_size);

        start = std::chrono::high_resolution_clock::now();
        for (size_t iter = 0; iter < iterations; ++iter) {
            auto get_fut = ep->get(local_region, 0, server_remote_addr, server_rkey, buffer_size);
            get_fut.get();
        }
        end = std::chrono::high_resolution_clock::now();
        auto get_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        auto get_total_bytes = static_cast<double>(buffer_size) * static_cast<double>(iterations);
        auto get_mbps = (get_us > 0) ? (get_total_bytes / static_cast<double>(get_us)) : 0.0;
        auto get_avg_us = static_cast<double>(get_us) / static_cast<double>(iterations);
        std::cout << "  Get " << buffer_size << " bytes x " << iterations
                  << " in " << get_us << " us"
                  << " (avg " << get_avg_us << " us/op, "
                  << get_mbps << " MB/s)\n";

        bool get_ok = true;
        for (size_t i = 0; i < buffer_size; ++i) {
            uint8_t expected = static_cast<uint8_t>(i & 0xFF);
            if (local_buf[i] != expected) {
                std::cout << "  Mismatch at offset " << i << "\n";
                get_ok = false;
                break;
            }
        }
        if (get_ok) {
            std::cout << "  RDMA get data verified OK\n";
        }

        if (!skip_atomics) {
        // ---- Atomic Fetch-and-Add ----
        std::cout << "\n--- Atomic Fetch-and-Add ---\n";
        for (int i = 0; i < 5; ++i) {
            auto atomic_fut = ep->atomic_fadd(server_remote_addr, server_rkey, 10);
            uint64_t old_val = atomic_fut.get();
            std::cout << "  fadd(10): old_value=" << old_val << " (expected " << (i * 10) << ")\n";
        }

        // ---- Atomic Compare-and-Swap ----
        std::cout << "\n--- Atomic Compare-and-Swap ---\n";
        // Reset: fadd(-50) to get back to 0
        auto reset_fut = ep->atomic_fadd(server_remote_addr, server_rkey,
                                          static_cast<uint64_t>(-50));
        reset_fut.get();

        // CAS: expected=0, desired=42
        auto cas_fut = ep->atomic_cswap(server_remote_addr, server_rkey, 0, 42);
        uint64_t cas_old = cas_fut.get();
        std::cout << "  cswap(expected=0, desired=42): old_value=" << cas_old << "\n";

        // Verify: CAS should fail now (value is 42, not 0)
        auto cas_fut2 = ep->atomic_cswap(server_remote_addr, server_rkey, 0, 99);
        uint64_t cas_old2 = cas_fut2.get();
        std::cout << "  cswap(expected=0, desired=99): old_value=" << cas_old2
                  << " (should be 42, CAS should fail)\n";
        }

        // Send "done" to server
        uint8_t sync_buf[1] = {1};
        auto sync_fut = ep->tag_send(sync_buf, sizeof(sync_buf), kSyncTag);
        sync_fut.get();

        worker->stop_progress_thread();
        std::cout << "\nClient done.\n";
    }

    return 0;
}
