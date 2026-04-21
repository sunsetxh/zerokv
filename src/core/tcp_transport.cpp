#include "core/tcp_transport.h"

#include "internal/logging.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace zerokv::core::detail {

namespace {

void log_tcp_error(const std::string& message) {
    ::zerokv::detail::write_log_line(::zerokv::detail::LogLevel::kError,
                                     "core.tcp",
                                     message);
}

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
        log_tcp_error("parse_address failed: invalid control address=" + address);
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
            log_tcp_error("parse_address failed: invalid control port address=" + address);
            return false;
        }
        port = (port * 10) + static_cast<uint64_t>(c - '0');
        if (port > std::numeric_limits<uint16_t>::max()) {
            if (error) {
                *error = "control port out of range";
            }
            log_tcp_error("parse_address failed: control port out of range address=" + address);
            return false;
        }
    }

    out->host = host;
    out->port = static_cast<uint16_t>(port);
    return true;
}

bool enable_tcp_nodelay(int fd, std::string* error) {
    int one = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        log_tcp_error(std::string("setsockopt(TCP_NODELAY) failed: ") + std::strerror(errno));
        return false;
    }
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
        log_tcp_error(std::string("socket failed in listen: ") + std::strerror(errno));
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
        log_tcp_error("listen failed: invalid IPv4 control host=" + parsed.host);
        ::close(fd);
        return -1;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        log_tcp_error(std::string("bind failed in listen: ") + std::strerror(errno));
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 64) != 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        log_tcp_error(std::string("listen syscall failed: ") + std::strerror(errno));
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
    if (fd >= 0 && !enable_tcp_nodelay(fd, error)) {
        ::close(fd);
        fd = -1;
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
        log_tcp_error(std::string("socket failed in connect: ") + std::strerror(errno));
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(parsed.port);
    if (::inet_pton(AF_INET, parsed.host.c_str(), &addr.sin_addr) != 1) {
        if (error) {
            *error = "invalid IPv4 control host";
        }
        log_tcp_error("connect failed: invalid IPv4 control host=" + parsed.host);
        ::close(fd);
        return -1;
    }

    if (timeout.count() < 0) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            if (error) {
                *error = std::strerror(errno);
            }
            log_tcp_error(std::string("connect failed: ") + std::strerror(errno));
            ::close(fd);
            return -1;
        }
        if (!enable_tcp_nodelay(fd, error)) {
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
        log_tcp_error(std::string("fcntl(F_GETFL) failed: ") + std::strerror(errno));
        ::close(fd);
        return -1;
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        log_tcp_error(std::string("fcntl(F_SETFL,O_NONBLOCK) failed: ") + std::strerror(errno));
        ::close(fd);
        return -1;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        (void)::fcntl(fd, F_SETFL, flags);
        if (!enable_tcp_nodelay(fd, error)) {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    if (errno != EINPROGRESS) {
        if (error) {
            *error = std::strerror(errno);
        }
        log_tcp_error(std::string("connect failed before poll: ") + std::strerror(errno));
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
        log_tcp_error("connect poll timed out");
        ::close(fd);
        return -1;
    }
    if (poll_rc < 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        log_tcp_error(std::string("connect poll failed: ") + std::strerror(errno));
        ::close(fd);
        return -1;
    }

    int so_error = 0;
    socklen_t so_error_len = sizeof(so_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) != 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        log_tcp_error(std::string("getsockopt(SO_ERROR) failed: ") + std::strerror(errno));
        ::close(fd);
        return -1;
    }
    if (so_error != 0) {
        if (error) {
            *error = std::strerror(so_error);
        }
        log_tcp_error(std::string("connect completed with socket error: ") + std::strerror(so_error));
        ::close(fd);
        return -1;
    }

    if (::fcntl(fd, F_SETFL, flags) != 0) {
        if (error) {
            *error = std::strerror(errno);
        }
        log_tcp_error(std::string("fcntl(F_SETFL,restore) failed: ") + std::strerror(errno));
        ::close(fd);
        return -1;
    }
    if (!enable_tcp_nodelay(fd, error)) {
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
            log_tcp_error(std::string("send_all failed: ") + std::strerror(errno));
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
            log_tcp_error(std::string("recv_exact failed: ") + std::strerror(errno));
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
