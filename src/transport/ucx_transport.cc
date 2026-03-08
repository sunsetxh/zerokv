#include "ucx_transport.h"
#include "zerokv/protocol.h"
#include <chrono>
#include <thread>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cstring>
#include <sstream>
#include <unistd.h>

namespace zerokv {

// Constants for validation
constexpr size_t MAX_KEY_SIZE = 1024;      // 1KB max key size
constexpr size_t MAX_VALUE_SIZE = 1024 * 1024;  // 1MB max value size
constexpr int RECV_TIMEOUT_SEC = 5;        // 5 second timeout

// Helper: send message with 4-byte length prefix
static bool send_with_length(int fd, const std::vector<uint8_t>& data) {
    uint32_t len = htonl(static_cast<uint32_t>(data.size()));
    if (send(fd, &len, sizeof(len), 0) != sizeof(len)) {
        return false;
    }
    if (send(fd, data.data(), data.size(), 0) != (ssize_t)data.size()) {
        return false;
    }
    return true;
}

// Helper: recv message with 4-byte length prefix
static bool recv_with_length(int fd, std::vector<uint8_t>& data) {
    uint32_t len;
    ssize_t n = recv(fd, &len, sizeof(len), 0);
    if (n != sizeof(len)) {
        return false;
    }
    len = ntohl(len);

    // Validate length
    if (len > MAX_VALUE_SIZE + 4096) {  // max message size
        return false;
    }

    data.resize(len);
    size_t received = 0;
    while (received < len) {
        n = recv(fd, data.data() + received, len - received, 0);
        if (n <= 0) {
            return false;
        }
        received += n;
    }
    return true;
}

// Global progress thread
static std::atomic<UCXTransport*> g_transport{nullptr};

UCXTransport::UCXTransport()
    : context_(nullptr), worker_(nullptr), listener_(nullptr),
      memory_handle_(nullptr), initialized_(false), is_server_(false),
      port_(0), running_(false), client_fd_(-1), server_fd_(-1) {
}

UCXTransport::~UCXTransport() {
    shutdown();
}

Status UCXTransport::initialize() {
    if (initialized_) {
        return Status::OK;
    }

    ucp_config_t* config;
    ucp_params_t params;
    ucs_status_t status;

    // Read UCX config from environment
    status = ucp_config_read("ZeroKV", nullptr, &config);
    if (status != UCS_OK) {
        std::cerr << "Failed to read UCX config: " << ucs_status_string(status) << std::endl;
        return Status::ERROR;
    }

    // Configure UCX for RDMA
    // Set default transport to TCP for now (RDMA requires hardware)
    setenv("UCX_NET_DEVICES", "tcp", 0);
    setenv("UCX_TLS", "tcp,cuda_copy", 0);

    // Initialize UCX context with features
    params.field_mask = UCP_PARAM_FIELD_FEATURES;
    params.features = UCP_FEATURE_TAG | UCP_FEATURE_RMA | UCP_FEATURE_AMO64;

    status = ucp_init(&params, config, &context_);
    ucp_config_release(config);

    if (status != UCS_OK) {
        std::cerr << "Failed to initialize UCX context: " << ucs_status_string(status) << std::endl;
        return Status::ERROR;
    }

    // Create worker
    ucp_worker_params_t worker_params;
    worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;

    status = ucp_worker_create(context_, &worker_params, &worker_);
    if (status != UCS_OK) {
        std::cerr << "Failed to create UCX worker: " << ucs_status_string(status) << std::endl;
        ucp_cleanup(context_);
        return Status::ERROR;
    }

    initialized_ = true;
    std::cout << "UCX transport initialized (TCP mode)" << std::endl;
    return Status::OK;
}

Status UCXTransport::listen(const std::string& addr, uint16_t port) {
    if (!initialized_) {
        return Status::ERROR;
    }

    local_addr_ = addr.empty() ? "0.0.0.0" : addr;
    port_ = port;
    is_server_ = true;

    // Create a TCP socket for listening
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return Status::ERROR;
    }

    // Set socket options
    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind to address
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_);
    inet_pton(AF_INET, local_addr_.c_str(), &server_addr.sin_addr);

    if (bind(server_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Failed to bind to " << local_addr_ << ":" << port_ << std::endl;
        close(server_fd_);
        return Status::ERROR;
    }

    // Listen for connections
    if (::listen(server_fd_, 5) < 0) {
        std::cerr << "Failed to listen" << std::endl;
        close(server_fd_);
        return Status::ERROR;
    }

    // Start accept thread for server
    running_ = true;
    accept_thread_ = std::thread([this]() { accept_loop(); });

    std::cout << "UCX listening on " << local_addr_ << ":" << port_ << std::endl;
    return Status::OK;
}

void UCXTransport::accept_loop() {
    while (running_) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (running_) {
                std::cerr << "Failed to accept connection" << std::endl;
            }
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "Accepted connection from " << client_ip << ":" << ntohs(client_addr.sin_port) << std::endl;

        // Handle client in a new thread
        std::thread([this, client_fd]() {
            handle_client(client_fd);
        }).detach();
    }
}

void UCXTransport::handle_client(int client_fd) {
    // Set receive timeout
    struct timeval tv;
    tv.tv_sec = RECV_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Handle multiple requests on the same connection
    while (true) {
        // Receive request with length prefix
        std::vector<uint8_t> request;
        if (!recv_with_length(client_fd, request)) {
            // Connection closed, error, or timeout
            break;
        }

        // Process request using protocol
        RequestHeader req_header;
        std::string key;
        void* value = nullptr;

        if (!ProtocolCodec::decode_request(request, req_header, key, value)) {
            // Send error response
            auto response = ProtocolCodec::encode_response(Status::ERROR, req_header.request_id);
            send_with_length(client_fd, response);
            break;
        }

        // Validate key length
        if (key.size() > MAX_KEY_SIZE) {
            auto response = ProtocolCodec::encode_response(Status::ERROR, req_header.request_id);
            send_with_length(client_fd, response);
            break;
        }

        // Validate value length
        if (req_header.value_len > MAX_VALUE_SIZE) {
            auto response = ProtocolCodec::encode_response(Status::OUT_OF_MEMORY, req_header.request_id);
            send_with_length(client_fd, response);
            break;
        }

        // Process the request based on opcode
        std::vector<uint8_t> response;

        switch (static_cast<OpCode>(req_header.opcode)) {
            case OpCode::PUT: {
                if (storage_) {
                    Status status = storage_->put(key, value, req_header.value_len);
                    response = ProtocolCodec::encode_response(status, req_header.request_id);
                } else {
                    response = ProtocolCodec::encode_response(Status::ERROR, req_header.request_id);
                }
                break;
            }
            case OpCode::GET: {
                if (storage_) {
                    std::vector<uint8_t> buffer(req_header.value_len > 0 ? req_header.value_len : 4096);
                    size_t size = buffer.size();
                    Status status = storage_->get(key, buffer.data(), &size);
                    if (status == Status::OK) {
                        response = ProtocolCodec::encode_response(status, req_header.request_id, buffer.data(), size);
                    } else {
                        response = ProtocolCodec::encode_response(status, req_header.request_id);
                    }
                } else {
                    response = ProtocolCodec::encode_response(Status::ERROR, req_header.request_id);
                }
                break;
            }
            case OpCode::DELETE: {
                if (storage_) {
                    Status status = storage_->delete_key(key);
                    response = ProtocolCodec::encode_response(status, req_header.request_id);
                } else {
                    response = ProtocolCodec::encode_response(Status::ERROR, req_header.request_id);
                }
                break;
            }
            default:
                response = ProtocolCodec::encode_response(Status::ERROR, req_header.request_id);
                break;
        }

        // Send response with length prefix
        if (!send_with_length(client_fd, response)) {
            break;
        }
    }

    close(client_fd);
}

Status UCXTransport::connect(const std::string& peer_addr) {
    if (!initialized_) {
        return Status::ERROR;
    }

    // Parse address (format: host:port)
    size_t colon_pos = peer_addr.find(':');
    std::string host = peer_addr.substr(0, colon_pos);
    uint16_t port = colon_pos != std::string::npos ?
        std::stoi(peer_addr.substr(colon_pos + 1)) : 5000;

    // Create socket
    client_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd_ < 0) {
        return Status::ERROR;
    }

    // Connect to server
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr);

    if (::connect(client_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(client_fd_);
        client_fd_ = -1;
        return Status::ERROR;
    }

    std::cout << "UCX connected to " << peer_addr << std::endl;
    return Status::OK;
}

Status UCXTransport::put(const std::string& key, const void* value, size_t size) {
    if (!initialized_) {
        return Status::ERROR;
    }

    if (client_fd_ < 0) {
        return Status::ERROR;
    }

    // Validate input
    if (key.size() > MAX_KEY_SIZE || size > MAX_VALUE_SIZE) {
        return Status::ERROR;
    }

    // Encode request
    auto request = ProtocolCodec::encode_request(
        OpCode::PUT, key, value, size, 0);

    // Send request with length prefix
    if (!send_with_length(client_fd_, request)) {
        return Status::ERROR;
    }

    // Receive response with length prefix
    std::vector<uint8_t> response;
    if (!recv_with_length(client_fd_, response)) {
        return Status::ERROR;
    }

    // Decode response
    ResponseHeader resp_header;
    void* resp_value = nullptr;
    size_t value_len = 0;

    if (!ProtocolCodec::decode_response(response, resp_header, resp_value, value_len)) {
        return Status::ERROR;
    }

    return static_cast<Status>(resp_header.status);
}

Status UCXTransport::get(const std::string& key, void* buffer, size_t* size) {
    if (!initialized_) {
        return Status::ERROR;
    }

    if (client_fd_ < 0) {
        return Status::ERROR;
    }

    // Validate input
    if (key.size() > MAX_KEY_SIZE) {
        return Status::ERROR;
    }

    // Encode request (value_len = 0 for GET)
    auto request = ProtocolCodec::encode_request(
        OpCode::GET, key, nullptr, 0, 0);

    // Send request with length prefix
    if (!send_with_length(client_fd_, request)) {
        return Status::ERROR;
    }

    // Receive response with length prefix
    std::vector<uint8_t> response;
    if (!recv_with_length(client_fd_, response)) {
        return Status::ERROR;
    }

    // Decode response
    ResponseHeader resp_header;
    void* resp_value = nullptr;
    size_t value_len = 0;

    if (!ProtocolCodec::decode_response(response, resp_header, resp_value, value_len)) {
        return Status::ERROR;
    }

    if (resp_header.status != 0) {  // NOT OK
        return static_cast<Status>(resp_header.status);
    }

    // Copy value to buffer
    if (buffer && value_len > 0) {
        memcpy(buffer, resp_value, std::min(value_len, *size));
    }
    *size = value_len;

    return Status::OK;
}

Status UCXTransport::put_user_mem(const std::string& key, void* remote_addr,
                                   uint32_t rkey, size_t size) {
    if (!initialized_) {
        return Status::ERROR;
    }
    return Status::OK;
}

Status UCXTransport::get_user_mem(const std::string& key, void* remote_addr,
                                   uint32_t rkey, size_t size) {
    if (!initialized_) {
        return Status::ERROR;
    }
    return Status::NOT_FOUND;
}

Status UCXTransport::delete_key(const std::string& key) {
    if (!initialized_) {
        return Status::ERROR;
    }

    if (client_fd_ < 0) {
        return Status::ERROR;
    }

    // Validate input
    if (key.size() > MAX_KEY_SIZE) {
        return Status::ERROR;
    }

    // Encode request
    auto request = ProtocolCodec::encode_request(
        OpCode::DELETE, key, nullptr, 0, 0);

    // Send request with length prefix
    if (!send_with_length(client_fd_, request)) {
        return Status::ERROR;
    }

    // Receive response with length prefix
    std::vector<uint8_t> response;
    if (!recv_with_length(client_fd_, response)) {
        return Status::ERROR;
    }

    // Decode response
    ResponseHeader resp_header;
    void* resp_value = nullptr;
    size_t value_len = 0;

    if (!ProtocolCodec::decode_response(response, resp_header, resp_value, value_len)) {
        return Status::ERROR;
    }

    return static_cast<Status>(resp_header.status);
}

void UCXTransport::shutdown() {
    if (!initialized_) {
        return;
    }

    running_ = false;

    // Stop accept thread
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    // Close sockets
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }
    if (client_fd_ >= 0) {
        close(client_fd_);
        client_fd_ = -1;
    }

    if (listener_) {
        ucp_listener_destroy(listener_);
        listener_ = nullptr;
    }

    if (worker_) {
        ucp_worker_destroy(worker_);
        worker_ = nullptr;
    }

    if (context_) {
        ucp_cleanup(context_);
        context_ = nullptr;
    }

    initialized_ = false;
    std::cout << "UCX transport shut down" << std::endl;
}

std::string UCXTransport::get_local_address() const {
    return local_addr_;
}

Status UCXTransport::register_memory(void* addr, size_t length, ucp_mem_h* mem_h) {
    if (!context_) {
        return Status::ERROR;
    }

    ucp_mem_map_params_t params;
    params.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS | UCP_MEM_MAP_PARAM_FIELD_LENGTH;
    params.address = addr;
    params.length = length;

    ucs_status_t status = ucp_mem_map(context_, &params, mem_h);
    if (status != UCS_OK) {
        std::cerr << "Failed to register memory: " << ucs_status_string(status) << std::endl;
        return Status::ERROR;
    }

    return Status::OK;
}

void UCXTransport::progress_loop() {
    while (running_) {
        if (worker_) {
            ucp_worker_progress(worker_);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

} // namespace zerokv
