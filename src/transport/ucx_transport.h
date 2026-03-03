#ifndef ZEROKV_UCX_TRANSPORT_H
#define ZEROKV_UCX_TRANSPORT_H

#include "zerokv/transport.h"
#include <ucp/api/ucp.h>
#include <ucs/sys/string.h>
#include <iostream>
#include <cstring>
#include <thread>
#include <atomic>
#include <map>
#include <mutex>

namespace zerokv {

// UCX Transport implementation with full RDMA support
class UCXTransport : public Transport {
public:
    UCXTransport();
    ~UCXTransport() override;

    Status initialize() override;
    Status listen(const std::string& addr, uint16_t port);
    Status connect(const std::string& peer_addr) override;
    Status put(const std::string& key, const void* value, size_t size) override;
    Status get(const std::string& key, void* buffer, size_t* size) override;
    Status put_user_mem(const std::string& key, void* remote_addr,
                        uint32_t rkey, size_t size) override;
    Status get_user_mem(const std::string& key, void* remote_addr,
                        uint32_t rkey, size_t size) override;
    Status delete_key(const std::string& key) override;
    void shutdown() override;

    // Get local address for sharing
    std::string get_local_address() const;
    uint16_t get_port() const { return port_; }

private:
    // UCX resources
    ucp_context_h context_;
    ucp_worker_h worker_;
    ucp_listener_h listener_;
    ucp_mem_h memory_handle_;

    // Connection management
    std::map<std::string, ucp_ep_h> endpoints_;
    std::mutex endpoint_mutex_;

    // State
    bool initialized_;
    bool is_server_;
    std::string local_addr_;
    uint16_t port_;
    std::atomic<bool> running_;
    std::thread progress_thread_;

    // Memory registration
    Status register_memory(void* addr, size_t length, ucp_mem_h* mem_h);
    Status get_remote_key(ucp_ep_h ep, void** rkey_buffer, size_t* rkey_size);

    // Progress loop
    void progress_loop();
};

} // namespace zerokv

#endif // ZEROKV_UCX_TRANSPORT_H
