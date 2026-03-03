#include "zerokv/client.h"
#include "zerokv/transport.h"
#include <iostream>

namespace zerokv {

Client::Client()
    : memory_type_(MemoryType::CPU), connected_(false), request_id_(0) {}

Client::~Client() {
    disconnect();
}

Status Client::connect(const std::vector<std::string>& servers) {
    if (connected_) {
        disconnect();
    }

    servers_ = servers;
    transport_ = create_transport(memory_type_);

    if (!transport_) {
        std::cerr << "Failed to create transport" << std::endl;
        return Status::ERROR;
    }

    Status status = transport_->initialize();
    if (status != Status::OK) {
        std::cerr << "Failed to initialize transport" << std::endl;
        return status;
    }

    // Connect to first server (simplified)
    if (!servers.empty()) {
        status = transport_->connect(servers[0]);
        if (status != Status::OK) {
            std::cerr << "Failed to connect to " << servers[0] << std::endl;
            return status;
        }
    }

    connected_ = true;
    std::cout << "Connected to ZeroKV cluster" << std::endl;
    return Status::OK;
}

void Client::disconnect() {
    if (transport_) {
        transport_->shutdown();
        transport_.reset();
    }
    connected_ = false;
}

Status Client::put(const std::string& key, const void* value, size_t size) {
    if (!connected_) return Status::ERROR;
    return transport_->put(key, value, size);
}

Status Client::put(const std::string& key, const std::string& value) {
    return put(key, value.data(), value.size());
}

Status Client::get(const std::string& key, void* buffer, size_t* size) {
    if (!connected_) return Status::ERROR;
    return transport_->get(key, buffer, size);
}

Status Client::get(const std::string& key, std::string* value) {
    if (!connected_) return Status::ERROR;

    // First get size
    size_t size = 0;
    Status status = transport_->get(key, nullptr, &size);
    if (status != Status::OK) return status;

    // Allocate buffer
    value->resize(size);
    return transport_->get(key, value->data(), &size);
}

Status Client::remove(const std::string& key) {
    if (!connected_) return Status::ERROR;
    return transport_->delete_key(key);
}

Status Client::put_user_mem(const std::string& key, void* remote_addr,
                            uint32_t rkey, size_t size) {
    if (!connected_) return Status::ERROR;
    return transport_->put_user_mem(key, remote_addr, rkey, size);
}

Status Client::get_user_mem(const std::string& key, void* remote_addr,
                            uint32_t rkey, size_t size) {
    if (!connected_) return Status::ERROR;
    return transport_->get_user_mem(key, remote_addr, rkey, size);
}

void Client::set_memory_type(MemoryType type) {
    memory_type_ = type;
}

} // namespace zerokv
