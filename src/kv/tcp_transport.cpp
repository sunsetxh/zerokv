#include "kv/tcp_transport.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace zerokv::core::detail {

namespace {

struct ParsedAddress {
    std::string host;
    uint16_t port = 0;
};

bool parse_address(const std::string& address, ParsedAddress* out, std::string* error) {
    const auto pos = address.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= address.size()) {
        if (error) {
            *error = "invalid control address";
        }
        return false;
    }

    const auto host = address.substr(0, pos);
    const auto port_str = address.substr(pos + 1);
    uint64_t port = 0;
    for (char c : port_str) {
        if (c < '0' || c > '9') {
            if (error) {
                *error = "invalid control port";
            }
            return false;
        }
        port = (port * 10) + static_cast<uint64_t>(c - '0');
        if (port > std::numeric_limits<uint16_t>::max()) {
            if (error) {
                *error = "control port out of range";
            }
            return false;
        }
    }

    out->host = host;
    out->port = static_cast<uint16_t>(port);
    return true;
}

}  // namespace

int TcpTransport::listen(const std::string& address, std::string* bound_address, std::string* error) {
    ParsedAddress parsed;
    if (!parse_address(address, &parsed, error)) {
        return -1;
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        return -1;
    }

    int reuse = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(parsed.port);
    if (::inet_pton(AF_INET, parsed.host.c_str(), &addr.sin_addr) != 1) {
        if (error) {
            *error = "invalid IPv4 control host";
        }
        ::close(fd);
        return -1;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 64) != 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        ::close(fd);
        return -1;
    }

    if (bound_address) {
        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
            char host_buf[INET_ADDRSTRLEN] = {0};
            if (::inet_ntop(AF_INET, &bound.sin_addr, host_buf, sizeof(host_buf))) {
                *bound_address = std::string(host_buf) + ":" +
                    std::to_string(static_cast<unsigned>(ntohs(bound.sin_port)));
            }
        }
    }

    return fd;
}

TcpTransport::Connection TcpTransport::accept(int listen_fd, std::string* error) {
    Connection conn;
    int fd = -1;
    do {
        fd = ::accept(listen_fd, nullptr, nullptr);
    } while (fd < 0 && errno == EINTR);

    if (fd < 0 && error) {
        *error = std::strerror(errno);
    }
    conn.fd = fd;
    return conn;
}

int TcpTransport::connect(const std::string& address, std::string* error) {
    return connect(address, std::chrono::milliseconds(-1), error);
}

int TcpTransport::connect(const std::string& address, std::chrono::milliseconds timeout, std::string* error) {
    ParsedAddress parsed;
    if (!parse_address(address, &parsed, error)) {
        return -1;
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(parsed.port);
    if (::inet_pton(AF_INET, parsed.host.c_str(), &addr.sin_addr) != 1) {
        if (error) {
            *error = "invalid IPv4 control host";
        }
        ::close(fd);
        return -1;
    }

    if (timeout.count() < 0) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            if (error) {
                *error = std::strerror(errno);
            }
            ::close(fd);
            return -1;
        }

        return fd;
    }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        ::close(fd);
        return -1;
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        ::close(fd);
        return -1;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        (void)::fcntl(fd, F_SETFL, flags);
        return fd;
    }

    if (errno != EINPROGRESS) {
        if (error) {
            *error = std::strerror(errno);
        }
        ::close(fd);
        return -1;
    }

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;

    int poll_rc = -1;
    do {
        poll_rc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
    } while (poll_rc < 0 && errno == EINTR);

    if (poll_rc == 0) {
        if (error) {
            *error = "timed out";
        }
        ::close(fd);
        return -1;
    }
    if (poll_rc < 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        ::close(fd);
        return -1;
    }

    int so_error = 0;
    socklen_t so_error_len = sizeof(so_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) != 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        ::close(fd);
        return -1;
    }
    if (so_error != 0) {
        if (error) {
            *error = std::strerror(so_error);
        }
        ::close(fd);
        return -1;
    }

    if (::fcntl(fd, F_SETFL, flags) != 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        ::close(fd);
        return -1;
    }

    return fd;
}

bool TcpTransport::send_all(int fd, std::span<const uint8_t> data) {
    size_t total = 0;
    while (total < data.size()) {
        const auto rc = ::send(fd, data.data() + total, data.size() - total, MSG_NOSIGNAL);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        total += static_cast<size_t>(rc);
    }
    return true;
}

bool TcpTransport::recv_exact(int fd, std::span<uint8_t> data) {
    size_t total = 0;
    while (total < data.size()) {
        const auto rc = ::recv(fd, data.data() + total, data.size() - total, 0);
        if (rc == 0) {
            return false;
        }
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        total += static_cast<size_t>(rc);
    }
    return true;
}

void TcpTransport::close_fd(int* fd) {
    if (!fd || *fd < 0) {
        return;
    }
    (void)::shutdown(*fd, SHUT_RDWR);
    (void)::close(*fd);
    *fd = -1;
}

}  // namespace zerokv::core::detail
