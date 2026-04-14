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

// ============================================================================
// Construction / destruction

AlpsKvChannel::AlpsKvChannel() = default;

AlpsKvChannel::~AlpsKvChannel() {
    Shutdown();
}

// ============================================================================
// Helpers

zerokv::Tag AlpsKvChannel::MakeMessageTag(int tag, int index, int src, int dst) {
    std::uint64_t hash = kFnvOffsetBasis;
    HashUint32(&hash, static_cast<std::uint32_t>(tag));
    HashUint32(&hash, static_cast<std::uint32_t>(index));
    HashUint32(&hash, static_cast<std::uint32_t>(src));
    HashUint32(&hash, static_cast<std::uint32_t>(dst));
    return hash == zerokv::kTagAny ? (hash - 1U) : hash;
}

zerokv::transport::MemoryRegion::Ptr AlpsKvChannel::GetOrRegisterSendRegion(
    SendCache& cache, const void* data, size_t size) {
    if (data == nullptr || size == 0 || !context_) {
        return nullptr;
    }
    const BufferKey key{data, size};
    auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }
    auto region = zerokv::transport::MemoryRegion::register_mem(
        context_, const_cast<void*>(data), size, zerokv::MemoryType::kHost);
    if (region) {
        cache.emplace(key, region);
    }
    return region;
}

bool AlpsKvChannel::InitContext() {
    // Use environment-driven config so UCX_TLS / UCX_NET_DEVICES / ZEROKV_TRANSPORT
    // are respected.  The builder default ("ucx") lets UCX auto-select the best
    // transport (e.g. RC/UD on RoCE).
    auto config = zerokv::Config::builder().from_env().build();
    context_ = zerokv::Context::create(config);
    if (!context_) {
        std::cerr << "AlpsKvChannel: failed to create zerokv context." << std::endl;
        return false;
    }
    return true;
}

// ============================================================================
// Listen mode (RANK0)

bool AlpsKvChannel::Listen(const std::string& bind_address, int connect_timeout_ms) {
    if (connect_timeout_ms <= 0) {
        std::cerr << "AlpsKvChannel::Listen: invalid connect timeout." << std::endl;
        return false;
    }

    Shutdown();
    if (!InitContext()) {
        return false;
    }

    recv_worker_ = zerokv::transport::Worker::create(context_);
    if (!recv_worker_) {
        std::cerr << "AlpsKvChannel::Listen: failed to create worker." << std::endl;
        context_.reset();
        return false;
    }

    running_ = true;
    mode_ = Mode::kListen;
    recv_worker_->start_progress_thread();

    listener_ = recv_worker_->listen(bind_address, [this](zerokv::transport::Endpoint::Ptr ep) {
        if (ep) {
            std::lock_guard<std::mutex> lock(endpoints_mutex_);
            endpoints_.push_back(std::move(ep));
            endpoints_cv_.notify_all();
        }
    });
    if (!listener_) {
        std::cerr << "AlpsKvChannel::Listen: failed to listen on " << bind_address << std::endl;
        Shutdown();
        return false;
    }

    local_address_ = listener_->address();
    return true;
}

zerokv::transport::Endpoint::Ptr AlpsKvChannel::WaitForAnyEndpoint(
    std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(endpoints_mutex_);
    auto pred = [this]() { return !running_ || !endpoints_.empty(); };
    if (timeout.count() < 0) {
        endpoints_cv_.wait(lock, pred);
    } else {
        endpoints_cv_.wait_for(lock, timeout, pred);
    }
    if (endpoints_.empty()) {
        return nullptr;
    }
    return endpoints_.front();
}

// ============================================================================
// Connect mode (RANK1)

bool AlpsKvChannel::Connect(const std::string& remote_address, int connect_timeout_ms) {
    if (remote_address.empty() || connect_timeout_ms <= 0) {
        std::cerr << "AlpsKvChannel::Connect: invalid arguments." << std::endl;
        return false;
    }

    Shutdown();
    if (!InitContext()) {
        return false;
    }

    running_ = true;
    mode_ = Mode::kConnect;
    remote_address_ = remote_address;
    connect_timeout_ms_ = connect_timeout_ms;
    return true;
}

AlpsKvChannel::PerThreadState* AlpsKvChannel::GetOrCreateThreadState() {
    const auto tid = std::this_thread::get_id();

    {
        std::lock_guard<std::mutex> lock(per_thread_mutex_);
        auto it = per_thread_states_.find(tid);
        if (it != per_thread_states_.end()) {
            return it->second.get();
        }
    }

    // Create a dedicated worker+endpoint for this thread.
    auto worker = zerokv::transport::Worker::create(context_);
    if (!worker) {
        std::cerr << "AlpsKvChannel: failed to create worker for thread." << std::endl;
        return nullptr;
    }
    worker->start_progress_thread();

    auto future = worker->connect(remote_address_);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(connect_timeout_ms_);

    while (running_) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            std::cerr << "AlpsKvChannel: timed out connecting to " << remote_address_ << std::endl;
            break;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        auto result = future.get(remaining);
        if (!result.has_value()) {
            if (!future.status().in_progress()) {
                std::cerr << "AlpsKvChannel: connect failed: "
                          << future.status().message() << std::endl;
                break;
            }
            continue;
        }
        if (!future.status().ok() || *result == nullptr) {
            std::cerr << "AlpsKvChannel: connect failed: "
                      << future.status().message() << std::endl;
            break;
        }

        // Flush to confirm the connection is live.
        auto flush = (*result)->flush();
        flush.get(std::chrono::milliseconds(connect_timeout_ms_));
        if (!flush.status().ok()) {
            std::cerr << "AlpsKvChannel: endpoint flush failed: "
                      << flush.status().message() << std::endl;
            break;
        }

        auto state = std::make_shared<PerThreadState>();
        state->worker = std::move(worker);
        state->endpoint = std::move(*result);

        std::lock_guard<std::mutex> lock(per_thread_mutex_);
        per_thread_states_[tid] = state;
        return state.get();
    }

    worker->stop_progress_thread();
    return nullptr;
}

// ============================================================================
// Shutdown

void AlpsKvChannel::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(endpoints_mutex_);
        running_ = false;
    }
    endpoints_cv_.notify_all();

    if (mode_ == Mode::kListen) {
        std::vector<zerokv::transport::Endpoint::Ptr> eps;
        {
            std::lock_guard<std::mutex> lock(endpoints_mutex_);
            eps = std::move(endpoints_);
        }
        for (auto& ep : eps) {
            auto f = ep->close();
            f.get(std::chrono::milliseconds(200));
        }
        if (listener_) {
            listener_->close();
            listener_.reset();
        }
        if (recv_worker_) {
            recv_worker_->stop_progress_thread();
            recv_worker_.reset();
        }
    } else if (mode_ == Mode::kConnect) {
        std::map<std::thread::id, std::shared_ptr<PerThreadState>> states;
        {
            std::lock_guard<std::mutex> lock(per_thread_mutex_);
            states = std::move(per_thread_states_);
        }
        for (auto& [tid, state] : states) {
            if (state->endpoint) {
                auto f = state->endpoint->close();
                f.get(std::chrono::milliseconds(200));
            }
            if (state->worker) {
                state->worker->stop_progress_thread();
            }
        }
    }

    context_.reset();
    mode_ = Mode::kUninitialized;
    local_address_.clear();
}

// ============================================================================
// WriteBytes

bool AlpsKvChannel::WriteBytes(const void* data, size_t size, int tag, int index, int src,
                                int dst) {
    if (data == nullptr && size > 0) {
        std::cerr << "AlpsKvChannel::WriteBytes: data is null." << std::endl;
        return false;
    }
    if (size == 0) {
        return true;
    }

    if (mode_ != Mode::kConnect) {
        std::cerr << "AlpsKvChannel::WriteBytes: not in connect mode." << std::endl;
        return false;
    }

    auto* state = GetOrCreateThreadState();
    if (!state) {
        std::cerr << "AlpsKvChannel::WriteBytes: no endpoint for thread." << std::endl;
        return false;
    }

    auto region = GetOrRegisterSendRegion(state->send_cache, data, size);
    auto future = region
        ? state->endpoint->tag_send(region, 0, size, MakeMessageTag(tag, index, src, dst))
        : state->endpoint->tag_send(data, size, MakeMessageTag(tag, index, src, dst));
    future.get();
    if (!future.status().ok()) {
        std::cerr << "AlpsKvChannel::WriteBytes: send failed: "
                  << future.status().message() << std::endl;
        return false;
    }
    return true;
}

// ============================================================================
// ReadBytes / ReadBytesBatch

void AlpsKvChannel::ReadBytes(void* data, size_t size, int tag, int index, int src, int dst) {
    if (data == nullptr || size == 0) {
        return;
    }
    if (mode_ != Mode::kListen) {
        std::cerr << "AlpsKvChannel::ReadBytes: not in listen mode." << std::endl;
        return;
    }

    auto ep = WaitForAnyEndpoint(std::chrono::milliseconds{-1});
    if (!ep) {
        std::cerr << "AlpsKvChannel::ReadBytes: no connected endpoint." << std::endl;
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
    if (count == 0) {
        return;
    }
    if (mode_ != Mode::kListen) {
        std::cerr << "AlpsKvChannel::ReadBytesBatch: not in listen mode." << std::endl;
        return;
    }

    // Wait for at least one connection.  Since tag_recv is worker-level,
    // one endpoint is sufficient to post recvs for messages from any sender.
    auto ep = WaitForAnyEndpoint(std::chrono::milliseconds{30000});
    if (!ep) {
        std::cerr << "AlpsKvChannel::ReadBytesBatch: no connected endpoint." << std::endl;
        return;
    }

    // Post all recvs in parallel, then wait for all.
    using RecvFuture = zerokv::transport::Future<std::pair<size_t, zerokv::Tag>>;
    std::vector<RecvFuture> futures;
    futures.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        futures.push_back(
            ep->tag_recv(data[i], sizes[i], MakeMessageTag(tags[i], indices[i], srcs[i], dsts[i])));
    }
    for (size_t i = 0; i < count; ++i) {
        futures[i].get();
        if (!futures[i].status().ok()) {
            std::cerr << "AlpsKvChannel::ReadBytesBatch: recv[" << i
                      << "] failed: " << futures[i].status().message() << std::endl;
        }
    }
}

// ============================================================================
// Accessors

std::string AlpsKvChannel::local_address() const {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    return local_address_;
}

}  // namespace zerokv::compat
