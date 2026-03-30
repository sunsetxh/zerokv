#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <span>
#include <string>
#include <vector>

namespace axon::kv::detail {

class TcpTransport {
public:
    struct Connection {
        int fd = -1;
    };

    static int listen(const std::string& address, std::string* bound_address, std::string* error);
    static Connection accept(int listen_fd, std::string* error);
    static int connect(const std::string& address, std::string* error);
    static int connect(const std::string& address, std::chrono::milliseconds timeout, std::string* error);

    static bool send_all(int fd, std::span<const uint8_t> data);
    static bool recv_exact(int fd, std::span<uint8_t> data);
    static void close_fd(int* fd);
};

}  // namespace axon::kv::detail
