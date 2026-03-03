#ifndef ZEROKV_SERVER_H
#define ZEROKV_SERVER_H

#include "zerokv/storage.h"
#include "zerokv/transport.h"
#include <memory>
#include <string>
#include <atomic>
#include <thread>
#include <vector>

// UCX types
#include <ucp/api/ucp.h>

namespace zerokv {

class Server {
public:
    Server();
    ~Server();

    Status start(const std::string& addr, uint16_t port, size_t max_memory);
    void stop();
    bool is_running() const { return running_; }

private:
    std::string addr_;
    uint16_t port_;
    std::unique_ptr<StorageEngine> storage_;
    std::atomic<bool> running_;
    std::thread worker_thread_;

    // UCX
    ucp_context_h context_;
    ucp_worker_h worker_;
    ucp_listener_h listener_;

    Status initialize_ucx();
    Status create_listener();
    void handle_request(ucp_worker_h worker, ucp_ep_h ep);
    void process_request(const std::vector<uint8_t>& request,
                         std::vector<uint8_t>& response);
    void run();
};

} // namespace zerokv

#endif // ZEROKV_SERVER_H
