#include "axon/config.h"

#include "axon/common.h"

#include <ucp/api/ucp.h>

#include <cstdlib>
#include <string>
#include <chrono>

namespace axon {

// ============================================================================
// Config::Impl
// ============================================================================

struct Config::Impl {
    std::string transport_ = "ucx";
    size_t num_workers_ = 0;  // 0 = auto
    size_t memory_pool_size_ = 64 * 1024 * 1024;
    size_t max_inflight_requests_ = 256;
    std::chrono::milliseconds connect_timeout_{10000};
    bool registration_cache_enabled_ = true;
    size_t registration_cache_max_entries_ = 1024;

    // UCX key-value options
    std::unordered_map<std::string, std::string> ucx_options_;
};

// ============================================================================
// Config::Builder::Impl
// ============================================================================

struct Config::Builder::Impl {
    std::string transport_ = "ucx";
    size_t num_workers_ = 0;
    size_t memory_pool_size_ = 64 * 1024 * 1024;
    size_t max_inflight_requests_ = 256;
    std::chrono::milliseconds connect_timeout_{10000};
    bool registration_cache_enabled_ = true;
    size_t registration_cache_max_entries_ = 1024;
    std::unordered_map<std::string, std::string> ucx_options_;
};

// ============================================================================
// Config::Builder
// ============================================================================

Config::Builder::Builder() : impl_(std::make_unique<Impl>()) {}

Config::Builder::~Builder() = default;

Config::Builder& Config::Builder::set_transport(std::string name) {
    impl_->transport_ = std::move(name);
    return *this;
}

Config::Builder& Config::Builder::set_num_workers(size_t n) {
    impl_->num_workers_ = n;
    return *this;
}

Config::Builder& Config::Builder::set_memory_pool_size(size_t bytes) {
    impl_->memory_pool_size_ = bytes;
    return *this;
}

Config::Builder& Config::Builder::set_max_inflight_requests(size_t n) {
    impl_->max_inflight_requests_ = n;
    return *this;
}

Config::Builder& Config::Builder::set_connect_timeout(std::chrono::milliseconds ms) {
    impl_->connect_timeout_ = ms;
    return *this;
}

Config::Builder& Config::Builder::enable_registration_cache(bool enable) {
    impl_->registration_cache_enabled_ = enable;
    return *this;
}

Config::Builder& Config::Builder::set_registration_cache_max_entries(size_t n) {
    impl_->registration_cache_max_entries_ = n;
    return *this;
}

Config::Builder& Config::Builder::set(std::string key, std::string value) {
    impl_->ucx_options_[std::move(key)] = std::move(value);
    return *this;
}

Config::Builder& Config::Builder::from_env() {
    // AXON-specific options
    if (auto* val = std::getenv("AXON_TRANSPORT")) {
        impl_->transport_ = val;
    }
    if (auto* val = std::getenv("AXON_NUM_WORKERS")) {
        impl_->num_workers_ = std::stoul(val);
    }
    if (auto* val = std::getenv("AXON_MEM_POOL_SIZE")) {
        impl_->memory_pool_size_ = std::stoul(val);
    }

    // UCX options - pass through
    if (auto* val = std::getenv("UCX_TLS")) {
        impl_->ucx_options_["UCX_TLS"] = val;
    }
    if (auto* val = std::getenv("UCX_NET_DEVICES")) {
        impl_->ucx_options_["UCX_NET_DEVICES"] = val;
    }
    if (auto* val = std::getenv("UCX_RNDV_THRESH")) {
        impl_->ucx_options_["UCX_RNDV_THRESH"] = val;
    }

    return *this;
}

Config Config::Builder::build() {
    // Transfer builder state to config
    Config config;
    config.impl_->transport_ = impl_->transport_;
    config.impl_->num_workers_ = impl_->num_workers_;
    config.impl_->memory_pool_size_ = impl_->memory_pool_size_;
    config.impl_->max_inflight_requests_ = impl_->max_inflight_requests_;
    config.impl_->connect_timeout_ = impl_->connect_timeout_;
    config.impl_->registration_cache_enabled_ = impl_->registration_cache_enabled_;
    config.impl_->registration_cache_max_entries_ = impl_->registration_cache_max_entries_;
    config.impl_->ucx_options_ = std::move(impl_->ucx_options_);
    return config;
}

// ============================================================================
// Config
// ============================================================================

Config::Config() : impl_(std::make_unique<Impl>()) {}

Config::~Config() = default;

Config::Config(const Config& other) : impl_(std::make_unique<Impl>()) {
    impl_->transport_ = other.impl_->transport_;
    impl_->num_workers_ = other.impl_->num_workers_;
    impl_->memory_pool_size_ = other.impl_->memory_pool_size_;
    impl_->max_inflight_requests_ = other.impl_->max_inflight_requests_;
    impl_->connect_timeout_ = other.impl_->connect_timeout_;
    impl_->registration_cache_enabled_ = other.impl_->registration_cache_enabled_;
    impl_->registration_cache_max_entries_ = other.impl_->registration_cache_max_entries_;
    impl_->ucx_options_ = other.impl_->ucx_options_;
}

Config& Config::operator=(const Config& other) {
    if (this != &other) {
        impl_->transport_ = other.impl_->transport_;
        impl_->num_workers_ = other.impl_->num_workers_;
        impl_->memory_pool_size_ = other.impl_->memory_pool_size_;
        impl_->max_inflight_requests_ = other.impl_->max_inflight_requests_;
        impl_->connect_timeout_ = other.impl_->connect_timeout_;
        impl_->registration_cache_enabled_ = other.impl_->registration_cache_enabled_;
        impl_->registration_cache_max_entries_ = other.impl_->registration_cache_max_entries_;
        impl_->ucx_options_ = other.impl_->ucx_options_;
    }
    return *this;
}

Config& Config::copy_from(const Config& other) {
    return *this = other;
}

Config::Config(Config&&) noexcept = default;
Config& Config::operator=(Config&&) noexcept = default;

const std::string& Config::transport() const noexcept { return impl_->transport_; }
size_t Config::num_workers() const noexcept { return impl_->num_workers_; }
size_t Config::memory_pool_size() const noexcept { return impl_->memory_pool_size_; }
size_t Config::max_inflight_requests() const noexcept { return impl_->max_inflight_requests_; }
std::chrono::milliseconds Config::connect_timeout() const noexcept { return impl_->connect_timeout_; }
bool Config::registration_cache_enabled() const noexcept { return impl_->registration_cache_enabled_; }
size_t Config::registration_cache_max_entries() const noexcept { return impl_->registration_cache_max_entries_; }

std::string Config::get(const std::string& key, const std::string& default_val) const {
    auto it = impl_->ucx_options_.find(key);
    if (it != impl_->ucx_options_.end()) {
        return it->second;
    }
    return default_val;
}

Config::Builder Config::builder() {
    return Builder{};
}

// ============================================================================
// Context
// ============================================================================

struct Context::Impl {
    Config config_;
    ucp_context_h handle_ = nullptr;

    ~Impl() {
        if (handle_) {
            ucp_cleanup(handle_);
        }
    }
};

Context::Context(const Config& config) : impl_(std::make_unique<Impl>()) {
    impl_->config_ = config;
}

Context::Ptr Context::create(const Config& config) {
    ucp_params_t params = {};
    params.field_mask = UCP_PARAM_FIELD_FEATURES |
                       UCP_PARAM_FIELD_REQUEST_SIZE |
                       UCP_PARAM_FIELD_MT_WORKERS_SHARED;  // Allow workers to be shared across threads

    // Enable RMA (Remote Memory Access), TAG matching, and AMO (Atomic Operations)
    params.features = UCP_FEATURE_TAG | UCP_FEATURE_STREAM | UCP_FEATURE_RMA | UCP_FEATURE_AMO64;

    params.request_size = 0;  // Use UCX default
    params.mt_workers_shared = 1;  // Enable multi-threaded worker access

    ucp_config_t* config_obj = nullptr;
    ucs_status_t config_status = ucp_config_read(nullptr, nullptr, &config_obj);
    if (config_status != UCS_OK) {
        return nullptr;
    }

    auto apply_option = [&](const std::string& key, const std::string& value) -> bool {
        return ucp_config_modify(config_obj, key.c_str(), value.c_str()) == UCS_OK;
    };

    const std::string transport = config.transport();
    if (transport == "tcp") {
        if (!apply_option("TLS", "tcp")) {
            ucp_config_release(config_obj);
            return nullptr;
        }
    } else if (transport == "shmem") {
        if (!apply_option("TLS", "sm")) {
            ucp_config_release(config_obj);
            return nullptr;
        }
    } else if (transport == "rdma") {
        if (!apply_option("TLS", "rc,sm,self")) {
            ucp_config_release(config_obj);
            return nullptr;
        }
    } else if (transport == "rdma_ud") {
        if (!apply_option("TLS", "ud,sm,self")) {
            ucp_config_release(config_obj);
            return nullptr;
        }
    } else if (!transport.empty() && transport != "ucx") {
        if (!apply_option("TLS", transport)) {
            ucp_config_release(config_obj);
            return nullptr;
        }
    }

    for (const auto& [key, value] : config.impl_->ucx_options_) {
        std::string option_key = key;
        constexpr std::string_view ucx_prefix = "UCX_";
        if (option_key.rfind(ucx_prefix, 0) == 0) {
            option_key.erase(0, ucx_prefix.size());
        }
        if (!apply_option(option_key, value)) {
            ucp_config_release(config_obj);
            return nullptr;
        }
    }

    ucp_context_h ctx = nullptr;
    ucs_status_t status = ucp_init(&params, config_obj, &ctx);

    if (status != UCS_OK) {
        ucp_config_release(config_obj);
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->config_ = config;
    impl->handle_ = ctx;

    ucp_config_release(config_obj);

    auto result = Ptr(new Context(config));
    result->impl_ = std::move(impl);
    return result;
}

Context::~Context() = default;

void* Context::native_handle() const noexcept {
    return impl_->handle_;
}

bool Context::supports_rma() const noexcept {
    // We always request UCP_FEATURE_RMA during context creation
    return impl_ && impl_->handle_;
}

bool Context::supports_memory_type(MemoryType type) const noexcept {
    switch (type) {
        case MemoryType::kHost:
            return true;
        case MemoryType::kCuda:
        case MemoryType::kRocm:
        case MemoryType::kAscend:
            return false;
    }
    return false;
}


const Config& Context::config() const noexcept {
    return impl_->config_;
}

bool Context::supports_hw_tag_matching() const noexcept {
    // UCX doesn't expose hardware tag matching capability directly
    // For now, return false - can be updated if UCX adds this query
    return false;
}

} // namespace axon
