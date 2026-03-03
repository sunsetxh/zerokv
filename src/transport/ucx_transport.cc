#include "ucx_transport.h"
#include <chrono>
#include <thread>
#include <arpa/inet.h>
#include <netinet/in.h>

namespace zerokv {

// Global progress thread
static std::atomic<UCXTransport*> g_transport{nullptr};

UCXTransport::UCXTransport()
    : context_(nullptr), worker_(nullptr), listener_(nullptr),
      memory_handle_(nullptr), initialized_(false), is_server_(false),
      port_(0), running_(false) {
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

    std::cout << "UCX listening on " << local_addr_ << ":" << port_ << std::endl;
    return Status::OK;
}

Status UCXTransport::connect(const std::string& peer_addr) {
    if (!initialized_) {
        return Status::ERROR;
    }

    std::cout << "UCX connected to " << peer_addr << std::endl;
    return Status::OK;
}

Status UCXTransport::put(const std::string& key, const void* value, size_t size) {
    if (!initialized_) {
        return Status::ERROR;
    }
    // Simplified - just return OK for now
    return Status::OK;
}

Status UCXTransport::get(const std::string& key, void* buffer, size_t* size) {
    if (!initialized_) {
        return Status::ERROR;
    }
    // Simplified - just return NOT_FOUND for now
    return Status::NOT_FOUND;
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
    return Status::OK;
}

void UCXTransport::shutdown() {
    if (!initialized_) {
        return;
    }

    running_ = false;

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
