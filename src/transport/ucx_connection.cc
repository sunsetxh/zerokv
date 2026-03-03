// UCX Connection Manager
#include <ucp/api/ucp.h>
#include <map>
#include <mutex>

class UCXConnectionManager {
public:
    struct Connection {
        ucp_ep_h ep;
        std::string peer_addr;
        bool is_connected;
    };
    
    bool add_connection(const std::string& peer, ucp_ep_h ep) {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_[peer] = {ep, peer, true};
        return true;
    }
    
    bool remove_connection(const std::string& peer) {
        std::lock_guard<std::mutex> lock(mutex_);
        return connections_.erase(peer) > 0;
    }
    
    ucp_ep_h get_connection(const std::string& peer) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connections_.find(peer);
        if (it != connections_.end()) {
            return it->second.ep;
        }
        return nullptr;
    }
    
private:
    std::map<std::string, Connection> connections_;
    std::mutex mutex_;
};
