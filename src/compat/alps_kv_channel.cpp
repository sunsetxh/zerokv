#include "compat/alps_kv_channel.h"

#include <chrono>
#include <cstring>
#include <iostream>

namespace zerokv::compat {

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void HashUint32(std::uint64_t* hash, std::uint32_t value) {
    *hash ^= value;
    *hash *= kFnvPrime;
}

}  // namespace

AlpsKvChannel::AlpsKvChannel() = default;

AlpsKvChannel::~AlpsKvChannel() {
    Shutdown();
}

bool AlpsKvChannel::InitializeTransport() {
    // Use environment-driven config so UCX_TLS / UCX_NET_DEVICES / ZEROKV_TRANSPORT
    // are respected.  The builder default ("ucx") lets UCX auto-select the best
    // transport (e.g. RC/UD on RoCE), avoiding the conflict that occurs when TLS
    // is forced to "tcp" while UCX_NET_DEVICES points to an RDMA device.
    auto config = zerokv::Config::builder().from_env().build();
    context_ = zerokv::Context::create(config);
    if (!context_) {
        std::cerr << "AlpsKvChannel: failed to create zerokv context." << std::endl;
        return false;
    }

    worker_ = zerokv::transport::Worker::create(context_);
    if (!worker_) {
        std::cerr << "AlpsKvChannel: failed to create zerokv worker." << std::endl;
        context_.reset();
        return false;
    }

    running_ = true;
    worker_->start_progress_thread();
    return true;
}

zerokv::Tag AlpsKvChannel::MakeMessageTag(int tag, int index, int src, int dst) {
    std::uint64_t hash = kFnvOffsetBasis;
    HashUint32(&hash, static_cast<std::uint32_t>(tag));
    HashUint32(&hash, static_cast<std::uint32_t>(index));
    HashUint32(&hash, static_cast<std::uint32_t>(src));
    HashUint32(&hash, static_cast<std::uint32_t>(dst));
    return hash == zerokv::kTagAny ? (hash - 1U) : hash;
}

zerokv::transport::Endpoint::Ptr AlpsKvChannel::WaitForConnectedEndpoint(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(state_mutex_);
    const bool ready = timeout.count() < 0
        ? (state_cv_.wait(lock, [this]() { return !running_ || connected_; }), connected_)
        : state_cv_.wait_for(lock, timeout, [this]() { return !running_ || connected_; });
    if (!ready || !connected_) {
        return nullptr;
    }
    return endpoint_;
}

zerokv::transport::MemoryRegion::Ptr AlpsKvChannel::GetOrRegisterSendRegion(const void* data, size_t size) {
    if (data == nullptr || size == 0 || !context_) {
        return nullptr;
    }

    const BufferKey key{data, size};
    std::lock_guard<std::mutex> lock(send_cache_mutex_);
    auto it = send_region_cache_.find(key);
    if (it != send_region_cache_.end()) {
        return it->second;
    }

    auto region = zerokv::transport::MemoryRegion::register_mem(
        context_, const_cast<void*>(data), size, zerokv::MemoryType::kHost);
    if (region) {
        send_region_cache_.emplace(key, region);
    }
    return region;
}

bool AlpsKvChannel::Listen(const std::string& bind_address, int connect_timeout_ms) {
    if (connect_timeout_ms <= 0) {
        std::cerr << "AlpsKvChannel::Listen: invalid connect timeout." << std::endl;
        return false;
    }

    Shutdown();
    if (!InitializeTransport()) {
        return false;
    }

    listener_ = worker_->listen(bind_address, [this](zerokv::transport::Endpoint::Ptr ep) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        endpoint_ = std::move(ep);
        connected_ = endpoint_ != nullptr;
        state_cv_.notify_all();
    });
    if (!listener_) {
        std::cerr << "AlpsKvChannel::Listen: failed to create listener on " << bind_address << std::endl;
        Shutdown();
        return false;
    }

    local_address_ = listener_->address();
    return true;
}

bool AlpsKvChannel::Connect(const std::string& remote_address, int connect_timeout_ms) {
    if (remote_address.empty() || connect_timeout_ms <= 0) {
        std::cerr << "AlpsKvChannel::Connect: invalid arguments." << std::endl;
        return false;
    }

    Shutdown();
    if (!InitializeTransport()) {
        return false;
    }

    auto future = worker_->connect(remote_address);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(connect_timeout_ms);
    while (running_) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            std::cerr << "AlpsKvChannel::Connect: timed out connecting to " << remote_address << std::endl;
            Shutdown();
            return false;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        auto result = future.get(remaining);
        if (!result.has_value()) {
            if (!future.status().in_progress()) {
                std::cerr << "AlpsKvChannel::Connect: connect failed: "
                          << future.status().message() << std::endl;
                Shutdown();
                return false;
            }
            continue;
        }
        if (!future.status().ok() || *result == nullptr) {
            std::cerr << "AlpsKvChannel::Connect: connect failed: "
                      << future.status().message() << std::endl;
            Shutdown();
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            endpoint_ = *result;
            connected_ = true;
            state_cv_.notify_all();
        }
        break;
    }

    auto ep = WaitForConnectedEndpoint(std::chrono::milliseconds(connect_timeout_ms));
    if (!ep) {
        std::cerr << "AlpsKvChannel::Connect: endpoint not ready." << std::endl;
        Shutdown();
        return false;
    }

    auto flush_future = ep->flush();
    auto flushed = flush_future.get(std::chrono::milliseconds(connect_timeout_ms));
    if (!flushed.has_value() || !flush_future.status().ok()) {
        std::cerr << "AlpsKvChannel::Connect: endpoint flush failed: "
                  << flush_future.status().message() << std::endl;
        Shutdown();
        return false;
    }

    return true;
}

void AlpsKvChannel::Shutdown() {
    zerokv::transport::Endpoint::Ptr endpoint;
    zerokv::transport::Worker::Ptr worker;
    zerokv::transport::Listener::Ptr listener;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        running_ = false;
        connected_ = false;
        endpoint = std::move(endpoint_);
        worker = std::move(worker_);
        listener = std::move(listener_);
    }
    state_cv_.notify_all();

    if (endpoint) {
        auto close_future = endpoint->close();
        close_future.get(std::chrono::milliseconds(200));
    }

    if (listener) {
        listener->close();
    }

    if (worker) {
        worker->stop_progress_thread();
    }

    context_.reset();
    local_address_.clear();
    {
        std::lock_guard<std::mutex> lock(send_cache_mutex_);
        send_region_cache_.clear();
    }
}

bool AlpsKvChannel::WriteBytes(const void* data, size_t size, int tag, int index, int src, int dst) {
    if (data == nullptr && size > 0) {
        std::cerr << "AlpsKvChannel::WriteBytes: data is null." << std::endl;
        return false;
    }
    if (size == 0) {
        return true;
    }

    auto ep = WaitForConnectedEndpoint(std::chrono::milliseconds(5000));
    if (!ep) {
        std::cerr << "AlpsKvChannel::WriteBytes: endpoint not connected." << std::endl;
        return false;
    }

    auto region = GetOrRegisterSendRegion(data, size);
    auto future = region
        ? ep->tag_send(region, 0, size, MakeMessageTag(tag, index, src, dst))
        : ep->tag_send(data, size, MakeMessageTag(tag, index, src, dst));
    future.get();
    if (!future.status().ok()) {
        std::cerr << "AlpsKvChannel::WriteBytes: send failed: "
                  << future.status().message() << std::endl;
        return false;
    }
    return true;
}

void AlpsKvChannel::ReadBytes(void* data, size_t size, int tag, int index, int src, int dst) {
    if (data == nullptr || size == 0) {
        return;
    }

    auto ep = WaitForConnectedEndpoint(std::chrono::milliseconds{-1});
    if (!ep) {
        std::cerr << "AlpsKvChannel::ReadBytes: endpoint not connected." << std::endl;
        return;
    }

    auto future = ep->tag_recv(data, size, MakeMessageTag(tag, index, src, dst));
    future.get();
    if (!future.status().ok()) {
        std::cerr << "AlpsKvChannel::ReadBytes: recv failed: "
                  << future.status().message() << std::endl;
    }
}

void AlpsKvChannel::ReadBytesBatch(std::vector<void*>& data,
                                   const std::vector<size_t>& sizes,
                                   const std::vector<int>& tags,
                                   const std::vector<int>& indices,
                                   const std::vector<int>& srcs,
                                   const std::vector<int>& dsts) {
    const size_t count = data.size();
    if (sizes.size() != count || tags.size() != count || indices.size() != count ||
        srcs.size() != count || dsts.size() != count) {
        std::cerr << "AlpsKvChannel::ReadBytesBatch: vector size mismatch." << std::endl;
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        ReadBytes(data[i], sizes[i], tags[i], indices[i], srcs[i], dsts[i]);
    }
}

std::string AlpsKvChannel::local_address() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return local_address_;
}

}  // namespace zerokv::compat
