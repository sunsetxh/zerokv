#include "compat/alps_kv_channel.h"

#include "core/tcp_framing.h"
#include "core/tcp_transport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <thread>
#include <vector>

namespace zerokv::compat {

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr zerokv::Tag kBootstrapControlPortTag = zerokv::kTagAny - 1U;

enum class AlpsControlType : uint16_t {
    kWriteRequest = 1001,
    kWriteGrant = 1002,
    kWriteDone = 1003,
    kWriteDoneAck = 1004,
};

struct WriteRequestPayload {
    zerokv::Tag message_tag = 0;
    uint64_t size = 0;
};

struct WriteGrantPayload {
    uint64_t reservation_id = 0;
    uint64_t remote_addr = 0;
    std::vector<uint8_t> rkey;
};

struct WriteDonePayload {
    uint64_t reservation_id = 0;
};

// Spin-wait on a future without calling ucp_worker_progress().
// Safe to use when a progress thread is already driving the worker.
template <typename F>
bool SpinUntilReady(F& future, std::chrono::steady_clock::time_point deadline) {
    while (!future.ready()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

template <typename F>
void SpinUntilReady(F& future) {
    while (!future.ready()) {
        std::this_thread::yield();
    }
}

void HashUint32(std::uint64_t* hash, std::uint32_t value) {
    *hash ^= value;
    *hash *= kFnvPrime;
}

void AppendU64(std::vector<uint8_t>* out, uint64_t value) {
    for (size_t i = 0; i < sizeof(value); ++i) {
        out->push_back(static_cast<uint8_t>((value >> (i * 8U)) & 0xffU));
    }
}

bool ReadU64(std::span<const uint8_t> data, size_t* offset, uint64_t* value) {
    if ((*offset + sizeof(uint64_t)) > data.size()) {
        return false;
    }
    uint64_t result = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        result |= static_cast<uint64_t>(data[*offset + i]) << (i * 8U);
    }
    *offset += sizeof(uint64_t);
    *value = result;
    return true;
}

void AppendBytes(std::vector<uint8_t>* out, std::span<const uint8_t> bytes) {
    AppendU64(out, bytes.size());
    out->insert(out->end(), bytes.begin(), bytes.end());
}

bool ReadBytes(std::span<const uint8_t> data, size_t* offset, std::vector<uint8_t>* bytes) {
    uint64_t length = 0;
    if (!ReadU64(data, offset, &length) || (*offset + length) > data.size()) {
        return false;
    }
    bytes->assign(data.begin() + static_cast<std::ptrdiff_t>(*offset),
                  data.begin() + static_cast<std::ptrdiff_t>(*offset + length));
    *offset += static_cast<size_t>(length);
    return true;
}

std::vector<uint8_t> EncodeWriteRequest(const WriteRequestPayload& payload) {
    std::vector<uint8_t> bytes;
    bytes.reserve(sizeof(uint64_t) * 2U);
    AppendU64(&bytes, payload.message_tag);
    AppendU64(&bytes, payload.size);
    return bytes;
}

std::optional<WriteRequestPayload> DecodeWriteRequest(std::span<const uint8_t> data) {
    WriteRequestPayload payload;
    size_t offset = 0;
    if (!ReadU64(data, &offset, &payload.message_tag) ||
        !ReadU64(data, &offset, &payload.size) ||
        offset != data.size()) {
        return std::nullopt;
    }
    return payload;
}

std::vector<uint8_t> EncodeWriteGrant(const WriteGrantPayload& payload) {
    std::vector<uint8_t> bytes;
    bytes.reserve((sizeof(uint64_t) * 3U) + payload.rkey.size());
    AppendU64(&bytes, payload.reservation_id);
    AppendU64(&bytes, payload.remote_addr);
    AppendBytes(&bytes, payload.rkey);
    return bytes;
}

std::optional<WriteGrantPayload> DecodeWriteGrant(std::span<const uint8_t> data) {
    WriteGrantPayload payload;
    size_t offset = 0;
    if (!ReadU64(data, &offset, &payload.reservation_id) ||
        !ReadU64(data, &offset, &payload.remote_addr) ||
        !ReadBytes(data, &offset, &payload.rkey) ||
        offset != data.size()) {
        return std::nullopt;
    }
    return payload;
}

std::vector<uint8_t> EncodeWriteDone(const WriteDonePayload& payload) {
    std::vector<uint8_t> bytes;
    bytes.reserve(sizeof(uint64_t));
    AppendU64(&bytes, payload.reservation_id);
    return bytes;
}

std::optional<WriteDonePayload> DecodeWriteDone(std::span<const uint8_t> data) {
    WriteDonePayload payload;
    size_t offset = 0;
    if (!ReadU64(data, &offset, &payload.reservation_id) || offset != data.size()) {
        return std::nullopt;
    }
    return payload;
}

bool SendControlFrame(int fd, AlpsControlType type, uint64_t request_id,
                      std::span<const uint8_t> payload) {
    return zerokv::core::detail::send_frame(
        fd,
        static_cast<zerokv::core::detail::MsgType>(static_cast<uint16_t>(type)),
        request_id,
        payload);
}

bool SendControlError(int fd, uint64_t request_id, const std::string& error) {
    const auto payload = std::vector<uint8_t>(error.begin(), error.end());
    return zerokv::core::detail::send_frame(
        fd, zerokv::core::detail::MsgType::kError, request_id, payload);
}

bool IsControlFrameType(const zerokv::core::detail::MsgHeader& header, AlpsControlType type) {
    return header.type == static_cast<uint16_t>(type);
}

std::string DecodeErrorPayload(std::span<const uint8_t> payload) {
    return std::string(payload.begin(), payload.end());
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

std::string AlpsKvChannel::ExtractHost(const std::string& address) {
    const auto pos = address.rfind(':');
    if (pos == std::string::npos) {
        return {};
    }
    return address.substr(0, pos);
}

std::string AlpsKvChannel::MakeAddress(const std::string& host, uint16_t port) {
    return host + ":" + std::to_string(port);
}

uint16_t AlpsKvChannel::ExtractPort(const std::string& address) {
    const auto pos = address.rfind(':');
    if (pos == std::string::npos || pos + 1 >= address.size()) {
        return 0;
    }
    const auto port_str = address.substr(pos + 1);
    uint64_t port = 0;
    for (char c : port_str) {
        if (c < '0' || c > '9') {
            return 0;
        }
        port = (port * 10U) + static_cast<uint64_t>(c - '0');
        if (port > std::numeric_limits<uint16_t>::max()) {
            return 0;
        }
    }
    return static_cast<uint16_t>(port);
}

bool AlpsKvChannel::InitContext() {
    // Use environment-driven config so UCX_TLS / UCX_NET_DEVICES / ZEROKV_TRANSPORT
    // are respected. The builder default ("ucx") lets UCX auto-select the best
    // transport (e.g. RC/UD on RoCE).
    auto config = zerokv::Config::builder().from_env().build();
    context_ = zerokv::Context::create(config);
    if (!context_) {
        std::cerr << "AlpsKvChannel: failed to create zerokv context." << std::endl;
        return false;
    }
    return true;
}

bool AlpsKvChannel::InitControlListener(const std::string& bind_address) {
    std::string error;
    const auto bind_host = ExtractHost(bind_address);
    control_listen_fd_ = zerokv::core::detail::TcpTransport::listen(
        MakeAddress(bind_host, 0), &control_address_, &error);
    if (control_listen_fd_ < 0) {
        std::cerr << "AlpsKvChannel: failed to listen for control traffic: "
                  << (error.empty() ? "unknown" : error) << std::endl;
        return false;
    }
    control_accept_thread_ = std::thread([this]() {
        ControlAcceptLoop();
    });
    return true;
}

void AlpsKvChannel::QueueBootstrapControlPort(const zerokv::transport::Endpoint::Ptr& ep) {
    if (!ep || !context_) {
        return;
    }

    const uint16_t control_port = ExtractPort(control_address_);
    if (control_port == 0) {
        return;
    }

    auto region = zerokv::transport::MemoryRegion::allocate(context_, sizeof(control_port));
    if (!region) {
        std::cerr << "AlpsKvChannel: failed to allocate bootstrap control region." << std::endl;
        return;
    }

    std::memcpy(region->address(), &control_port, sizeof(control_port));
    auto future = ep->tag_send(region, 0, sizeof(control_port), kBootstrapControlPortTag);
    if (!future.status().ok() && future.status().code() != zerokv::ErrorCode::kInProgress) {
        std::cerr << "AlpsKvChannel: failed to send control bootstrap tag: "
                  << future.status().message() << std::endl;
        return;
    }

    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    pending_bootstrap_sends_.push_back(PendingBootstrapSend{
        .future = std::move(future),
        .region = std::move(region),
    });
}

void AlpsKvChannel::ReapBootstrapSends() {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    pending_bootstrap_sends_.erase(
        std::remove_if(pending_bootstrap_sends_.begin(), pending_bootstrap_sends_.end(),
                       [](PendingBootstrapSend& pending) {
                           return pending.future.ready();
                       }),
        pending_bootstrap_sends_.end());
}

std::shared_ptr<AlpsKvChannel::ReceiveSlot> AlpsKvChannel::RegisterReceiveSlot(
    void* data, size_t size, zerokv::Tag message_tag) {
    auto region = zerokv::transport::MemoryRegion::register_mem(
        context_, data, size, zerokv::MemoryType::kHost);
    if (!region) {
        std::cerr << "AlpsKvChannel: failed to register receive buffer." << std::endl;
        return nullptr;
    }

    auto slot = std::make_shared<ReceiveSlot>();
    slot->region = std::move(region);
    slot->remote_key = slot->region->remote_key();
    slot->size = size;
    slot->reservation_id = next_reservation_id_.fetch_add(1);

    {
        std::lock_guard<std::mutex> lock(receive_slots_mutex_);
        if (!receive_slots_.emplace(message_tag, slot).second) {
            std::cerr << "AlpsKvChannel: duplicate outstanding receive tag " << message_tag
                      << " is not supported." << std::endl;
            return nullptr;
        }
        receive_slots_cv_.notify_all();
    }
    TryDeliverBufferedMessage(message_tag);
    return slot;
}

void AlpsKvChannel::FinishReceiveSlot(const std::shared_ptr<ReceiveSlot>& slot,
                                      const std::string& error) {
    if (!slot) {
        return;
    }

    std::lock_guard<std::mutex> lock(slot->mutex);
    if (slot->done) {
        return;
    }
    slot->done = true;
    slot->success = error.empty();
    slot->error = error;
    slot->cv.notify_all();
}

void AlpsKvChannel::RemoveReceiveSlot(zerokv::Tag message_tag,
                                      const std::shared_ptr<ReceiveSlot>& slot) {
    std::lock_guard<std::mutex> lock(receive_slots_mutex_);
    auto it = receive_slots_.find(message_tag);
    if (it != receive_slots_.end() && it->second == slot) {
        receive_slots_.erase(it);
    }
    receive_slots_cv_.notify_all();
}

bool AlpsKvChannel::WaitForSlotCompletion(const std::shared_ptr<ReceiveSlot>& slot) {
    if (!slot) {
        return false;
    }

    std::unique_lock<std::mutex> lock(slot->mutex);
    slot->cv.wait(lock, [this, &slot]() {
        return !running_ || slot->done;
    });
    return slot->done && slot->success;
}

void AlpsKvChannel::TryDeliverBufferedMessage(zerokv::Tag message_tag) {
    std::shared_ptr<BufferedMessage> buffered;
    std::shared_ptr<ReceiveSlot> slot;
    {
        std::lock_guard<std::mutex> lock(receive_slots_mutex_);
        auto buffered_it = staged_messages_.find(message_tag);
        auto slot_it = receive_slots_.find(message_tag);
        if (buffered_it == staged_messages_.end() || slot_it == receive_slots_.end() ||
            !buffered_it->second || !slot_it->second || !buffered_it->second->completed) {
            return;
        }
        buffered = buffered_it->second;
        slot = slot_it->second;
        staged_messages_.erase(buffered_it);
    }

    if (buffered->size != slot->size) {
        FinishReceiveSlot(slot, "staged ALPS payload size mismatch");
        return;
    }

    std::memcpy(slot->region->address(), buffered->region->address(), buffered->size);
    FinishReceiveSlot(slot, {});
}

void AlpsKvChannel::ControlAcceptLoop() {
    while (running_) {
        std::string error;
        auto conn = zerokv::core::detail::TcpTransport::accept(control_listen_fd_, &error);
        if (conn.fd < 0) {
            if (!running_) {
                break;
            }
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(active_control_fds_mutex_);
            active_control_fds_.insert(conn.fd);
        }
        std::lock_guard<std::mutex> lock(control_threads_mutex_);
        control_threads_.emplace_back([this, fd = conn.fd]() {
            ControlConnectionLoop(fd);
        });
    }
}

void AlpsKvChannel::ControlConnectionLoop(int fd) {
    const int tracked_fd = fd;
    while (running_) {
        zerokv::core::detail::MsgHeader header;
        std::vector<uint8_t> payload;
        if (!zerokv::core::detail::recv_frame(fd, &header, &payload)) {
            break;
        }

        if (!IsControlFrameType(header, AlpsControlType::kWriteRequest)) {
            if (!SendControlError(fd, header.request_id, "unexpected ALPS control frame")) {
                break;
            }
            continue;
        }

        auto request = DecodeWriteRequest(payload);
        if (!request.has_value()) {
            if (!SendControlError(fd, header.request_id, "failed to decode ALPS write request")) {
                break;
            }
            continue;
        }

        std::shared_ptr<ReceiveSlot> slot;
        std::shared_ptr<BufferedMessage> buffered;
        {
            std::lock_guard<std::mutex> lock(receive_slots_mutex_);
            auto slot_it = receive_slots_.find(request->message_tag);
            if (slot_it != receive_slots_.end() && slot_it->second &&
                !slot_it->second->reserved && !slot_it->second->done) {
                if (slot_it->second->size != request->size) {
                    if (!SendControlError(fd, header.request_id, "receive buffer size mismatch")) {
                        break;
                    }
                    continue;
                }
                slot_it->second->reserved = true;
                slot = slot_it->second;
            } else if (staged_messages_.find(request->message_tag) != staged_messages_.end()) {
                if (!SendControlError(fd, header.request_id, "duplicate outstanding ALPS message")) {
                    break;
                }
                continue;
            }
        }

        if (!slot) {
            auto region = zerokv::transport::MemoryRegion::allocate(
                context_, static_cast<size_t>(request->size));
            if (!region) {
                if (!SendControlError(fd, header.request_id, "failed to allocate ALPS staging buffer")) {
                    break;
                }
                continue;
            }

            buffered = std::make_shared<BufferedMessage>();
            buffered->region = std::move(region);
            buffered->remote_key = buffered->region->remote_key();
            buffered->size = static_cast<size_t>(request->size);
            buffered->reservation_id = next_reservation_id_.fetch_add(1);

            std::lock_guard<std::mutex> lock(receive_slots_mutex_);
            if (staged_messages_.find(request->message_tag) != staged_messages_.end()) {
                if (!SendControlError(fd, header.request_id, "duplicate outstanding ALPS message")) {
                    break;
                }
                continue;
            }
            staged_messages_.emplace(request->message_tag, buffered);
        }

        WriteGrantPayload grant;
        grant.reservation_id = slot ? slot->reservation_id : buffered->reservation_id;
        grant.remote_addr = reinterpret_cast<uint64_t>(
            slot ? slot->region->address() : buffered->region->address());
        grant.rkey = slot ? slot->remote_key.data : buffered->remote_key.data;
        const auto grant_payload = EncodeWriteGrant(grant);
        if (!SendControlFrame(fd, AlpsControlType::kWriteGrant, header.request_id, grant_payload)) {
            if (slot) {
                FinishReceiveSlot(slot, "failed to send ALPS write grant");
            } else {
                std::lock_guard<std::mutex> lock(receive_slots_mutex_);
                staged_messages_.erase(request->message_tag);
            }
            break;
        }

        zerokv::core::detail::MsgHeader done_header;
        std::vector<uint8_t> done_payload;
        if (!zerokv::core::detail::recv_frame(fd, &done_header, &done_payload)) {
            if (slot) {
                FinishReceiveSlot(slot, "writer disconnected before write completion");
            } else {
                std::lock_guard<std::mutex> lock(receive_slots_mutex_);
                staged_messages_.erase(request->message_tag);
            }
            break;
        }

        if (!IsControlFrameType(done_header, AlpsControlType::kWriteDone) ||
            done_header.request_id != header.request_id) {
            FinishReceiveSlot(slot, "unexpected ALPS write completion frame");
            if (!SendControlError(fd, done_header.request_id, "unexpected ALPS write completion frame")) {
                break;
            }
            continue;
        }

        auto done = DecodeWriteDone(done_payload);
        const uint64_t expected_reservation_id =
            slot ? slot->reservation_id : buffered->reservation_id;
        if (!done.has_value() || done->reservation_id != expected_reservation_id) {
            if (slot) {
                FinishReceiveSlot(slot, "invalid ALPS reservation completion");
            } else {
                std::lock_guard<std::mutex> lock(receive_slots_mutex_);
                staged_messages_.erase(request->message_tag);
            }
            if (!SendControlError(fd, done_header.request_id, "invalid ALPS reservation completion")) {
                break;
            }
            continue;
        }

        if (slot) {
            FinishReceiveSlot(slot, {});
        } else {
            buffered->completed = true;
            TryDeliverBufferedMessage(request->message_tag);
        }
        const auto ack_payload = EncodeWriteDone(*done);
        if (!SendControlFrame(fd, AlpsControlType::kWriteDoneAck, header.request_id, ack_payload)) {
            break;
        }
    }

    zerokv::core::detail::TcpTransport::close_fd(&fd);
    std::lock_guard<std::mutex> lock(active_control_fds_mutex_);
    active_control_fds_.erase(tracked_fd);
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

    if (!InitControlListener(bind_address)) {
        Shutdown();
        return false;
    }

    listener_ = recv_worker_->listen(bind_address, [this](zerokv::transport::Endpoint::Ptr ep) {
        if (!ep) {
            return;
        }
        QueueBootstrapControlPort(ep);
        std::lock_guard<std::mutex> lock(endpoints_mutex_);
        endpoints_.push_back(std::move(ep));
        endpoints_cv_.notify_all();
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

bool AlpsKvChannel::BootstrapControlAddress(PerThreadState* state,
                                            std::chrono::steady_clock::time_point deadline) {
    if (state == nullptr) {
        return false;
    }
    if (!state->control_address.empty()) {
        return true;
    }

    uint16_t control_port = 0;
    auto future = state->endpoint->tag_recv(&control_port, sizeof(control_port),
                                            kBootstrapControlPortTag);
    if (!SpinUntilReady(future, deadline)) {
        std::cerr << "AlpsKvChannel: timed out waiting for control bootstrap tag." << std::endl;
        return false;
    }
    if (!future.status().ok()) {
        std::cerr << "AlpsKvChannel: failed to recv control bootstrap tag: "
                  << future.status().message() << std::endl;
        return false;
    }
    if (control_port == 0) {
        std::cerr << "AlpsKvChannel: server returned invalid control port." << std::endl;
        return false;
    }

    state->control_address = MakeAddress(ExtractHost(remote_address_), control_port);
    return true;
}

bool AlpsKvChannel::EnsureControlConnection(PerThreadState* state) {
    if (state == nullptr) {
        return false;
    }
    if (state->control_fd >= 0) {
        return true;
    }

    std::string error;
    state->control_fd = zerokv::core::detail::TcpTransport::connect(
        state->control_address,
        std::chrono::milliseconds(connect_timeout_ms_),
        &error);
    if (state->control_fd < 0) {
        std::cerr << "AlpsKvChannel: failed to connect to control channel "
                  << state->control_address << ": "
                  << (error.empty() ? "unknown" : error) << std::endl;
        return false;
    }
    return true;
}

void AlpsKvChannel::CloseControlFd(int* fd) {
    zerokv::core::detail::TcpTransport::close_fd(fd);
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

    auto worker = zerokv::transport::Worker::create(context_);
    if (!worker) {
        std::cerr << "AlpsKvChannel: failed to create worker for thread." << std::endl;
        return nullptr;
    }
    worker->start_progress_thread();

    auto future = worker->connect(remote_address_);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(connect_timeout_ms_);

    if (!SpinUntilReady(future, deadline)) {
        std::cerr << "AlpsKvChannel: timed out connecting to " << remote_address_ << std::endl;
        worker->stop_progress_thread();
        return nullptr;
    }
    auto conn_result = future.get();
    if (!future.status().ok() || conn_result == nullptr) {
        std::cerr << "AlpsKvChannel: connect failed: " << future.status().message() << std::endl;
        worker->stop_progress_thread();
        return nullptr;
    }

    auto flush = conn_result->flush();
    if (!SpinUntilReady(flush, deadline)) {
        std::cerr << "AlpsKvChannel: endpoint flush timed out." << std::endl;
        worker->stop_progress_thread();
        return nullptr;
    }
    if (!flush.status().ok()) {
        std::cerr << "AlpsKvChannel: endpoint flush failed: " << flush.status().message()
                  << std::endl;
        worker->stop_progress_thread();
        return nullptr;
    }

    auto state = std::make_shared<PerThreadState>();
    state->worker = std::move(worker);
    state->endpoint = std::move(conn_result);
    if (!BootstrapControlAddress(state.get(), deadline)) {
        auto close = state->endpoint->close();
        close.get(std::chrono::milliseconds(200));
        state->worker->stop_progress_thread();
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(per_thread_mutex_);
    per_thread_states_[tid] = state;
    return state.get();
}

// ============================================================================
// Shutdown

void AlpsKvChannel::Shutdown() {
    running_ = false;
    endpoints_cv_.notify_all();
    receive_slots_cv_.notify_all();

    std::vector<std::shared_ptr<ReceiveSlot>> slots;
    {
        std::lock_guard<std::mutex> lock(receive_slots_mutex_);
        for (const auto& [tag, slot] : receive_slots_) {
            slots.push_back(slot);
        }
        receive_slots_.clear();
        staged_messages_.clear();
    }
    for (const auto& slot : slots) {
        FinishReceiveSlot(slot, "channel shutdown");
    }

    if (control_listen_fd_ >= 0) {
        CloseControlFd(&control_listen_fd_);
    }
    {
        std::lock_guard<std::mutex> lock(active_control_fds_mutex_);
        for (int fd : active_control_fds_) {
            int fd_to_close = fd;
            CloseControlFd(&fd_to_close);
        }
        active_control_fds_.clear();
    }
    if (control_accept_thread_.joinable()) {
        control_accept_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(control_threads_mutex_);
        for (auto& thread : control_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        control_threads_.clear();
    }

    if (mode_ == Mode::kListen) {
        std::vector<zerokv::transport::Endpoint::Ptr> eps;
        {
            std::lock_guard<std::mutex> lock(endpoints_mutex_);
            eps = std::move(endpoints_);
            pending_bootstrap_sends_.clear();
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
            CloseControlFd(&state->control_fd);
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
    control_address_.clear();
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
    if (!EnsureControlConnection(state)) {
        return false;
    }

    const auto message_tag = MakeMessageTag(tag, index, src, dst);
    const auto request_id = state->next_request_id++;
    const auto write_request = EncodeWriteRequest(WriteRequestPayload{
        .message_tag = message_tag,
        .size = size,
    });
    if (!SendControlFrame(state->control_fd, AlpsControlType::kWriteRequest,
                          request_id, write_request)) {
        CloseControlFd(&state->control_fd);
        std::cerr << "AlpsKvChannel::WriteBytes: failed to send control write request." << std::endl;
        return false;
    }

    zerokv::core::detail::MsgHeader grant_header;
    std::vector<uint8_t> grant_payload;
    if (!zerokv::core::detail::recv_frame(state->control_fd, &grant_header, &grant_payload)) {
        CloseControlFd(&state->control_fd);
        std::cerr << "AlpsKvChannel::WriteBytes: failed to read control write grant." << std::endl;
        return false;
    }
    if (grant_header.request_id != request_id) {
        CloseControlFd(&state->control_fd);
        std::cerr << "AlpsKvChannel::WriteBytes: mismatched control request id." << std::endl;
        return false;
    }
    if (grant_header.type == static_cast<uint16_t>(zerokv::core::detail::MsgType::kError)) {
        CloseControlFd(&state->control_fd);
        std::cerr << "AlpsKvChannel::WriteBytes: control error: "
                  << DecodeErrorPayload(grant_payload) << std::endl;
        return false;
    }
    if (!IsControlFrameType(grant_header, AlpsControlType::kWriteGrant)) {
        CloseControlFd(&state->control_fd);
        std::cerr << "AlpsKvChannel::WriteBytes: unexpected control write grant frame." << std::endl;
        return false;
    }

    auto grant = DecodeWriteGrant(grant_payload);
    if (!grant.has_value()) {
        CloseControlFd(&state->control_fd);
        std::cerr << "AlpsKvChannel::WriteBytes: failed to decode write grant." << std::endl;
        return false;
    }

    auto region = GetOrRegisterSendRegion(state->send_cache, data, size);
    if (!region) {
        CloseControlFd(&state->control_fd);
        std::cerr << "AlpsKvChannel::WriteBytes: failed to register send buffer." << std::endl;
        return false;
    }

    zerokv::transport::RemoteKey remote_key;
    remote_key.data = grant->rkey;
    auto put = state->endpoint->put(region, 0, grant->remote_addr, remote_key, size);
    SpinUntilReady(put);
    if (!put.status().ok()) {
        CloseControlFd(&state->control_fd);
        std::cerr << "AlpsKvChannel::WriteBytes: put failed: " << put.status().message()
                  << std::endl;
        return false;
    }

    auto flush = state->endpoint->flush();
    SpinUntilReady(flush);
    if (!flush.status().ok()) {
        CloseControlFd(&state->control_fd);
        std::cerr << "AlpsKvChannel::WriteBytes: flush failed: " << flush.status().message()
                  << std::endl;
        return false;
    }

    const auto write_done = EncodeWriteDone(WriteDonePayload{
        .reservation_id = grant->reservation_id,
    });
    if (!SendControlFrame(state->control_fd, AlpsControlType::kWriteDone,
                          request_id, write_done)) {
        CloseControlFd(&state->control_fd);
        std::cerr << "AlpsKvChannel::WriteBytes: failed to send write completion." << std::endl;
        return false;
    }

    zerokv::core::detail::MsgHeader ack_header;
    std::vector<uint8_t> ack_payload;
    if (!zerokv::core::detail::recv_frame(state->control_fd, &ack_header, &ack_payload)) {
        CloseControlFd(&state->control_fd);
        std::cerr << "AlpsKvChannel::WriteBytes: failed to read write completion ack." << std::endl;
        return false;
    }
    if (ack_header.request_id != request_id) {
        CloseControlFd(&state->control_fd);
        std::cerr << "AlpsKvChannel::WriteBytes: mismatched write completion ack id." << std::endl;
        return false;
    }
    if (ack_header.type == static_cast<uint16_t>(zerokv::core::detail::MsgType::kError)) {
        CloseControlFd(&state->control_fd);
        std::cerr << "AlpsKvChannel::WriteBytes: completion ack error: "
                  << DecodeErrorPayload(ack_payload) << std::endl;
        return false;
    }
    if (!IsControlFrameType(ack_header, AlpsControlType::kWriteDoneAck)) {
        CloseControlFd(&state->control_fd);
        std::cerr << "AlpsKvChannel::WriteBytes: unexpected write completion ack frame." << std::endl;
        return false;
    }

#ifdef ZEROKV_ALPS_TEST_HOOKS
    ++rma_put_ops_;
#endif
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

    const auto message_tag = MakeMessageTag(tag, index, src, dst);
    auto slot = RegisterReceiveSlot(data, size, message_tag);
    if (!slot) {
        return;
    }

    if (!WaitForSlotCompletion(slot)) {
        std::cerr << "AlpsKvChannel::ReadBytes: receive failed: "
                  << (slot->error.empty() ? "unknown" : slot->error) << std::endl;
    }
    RemoveReceiveSlot(message_tag, slot);
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

    std::vector<std::pair<zerokv::Tag, std::shared_ptr<ReceiveSlot>>> slots;
    slots.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto message_tag = MakeMessageTag(tags[i], indices[i], srcs[i], dsts[i]);
        auto slot = RegisterReceiveSlot(data[i], sizes[i], message_tag);
        if (!slot) {
            for (auto& [posted_tag, posted_slot] : slots) {
                FinishReceiveSlot(posted_slot, "batch setup aborted");
                RemoveReceiveSlot(posted_tag, posted_slot);
            }
            return;
        }
        slots.emplace_back(message_tag, std::move(slot));
    }

    for (size_t i = 0; i < count; ++i) {
        if (!WaitForSlotCompletion(slots[i].second)) {
            std::cerr << "AlpsKvChannel::ReadBytesBatch: recv[" << i
                      << "] failed: "
                      << (slots[i].second->error.empty() ? "unknown" : slots[i].second->error)
                      << std::endl;
        }
        RemoveReceiveSlot(slots[i].first, slots[i].second);
    }
}

// ============================================================================
// Accessors

std::string AlpsKvChannel::local_address() const {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    return local_address_;
}

#ifdef ZEROKV_ALPS_TEST_HOOKS
AlpsKvChannel::DebugStats AlpsKvChannel::debug_stats() const {
    return DebugStats{
        .payload_tag_send_ops = payload_tag_send_ops_.load(),
        .rma_put_ops = rma_put_ops_.load(),
    };
}
#endif

}  // namespace zerokv::compat
