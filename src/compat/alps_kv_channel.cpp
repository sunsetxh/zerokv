#include "compat/alps_kv_channel.h"

#include "core/tcp_framing.h"
#include "core/tcp_transport.h"

#include <ucp/api/ucp.h>

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

using detail::WriteDonePayload;
using detail::WriteGrantPayload;
using detail::WriteRequestPayload;

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr zerokv::Tag kBootstrapControlPortTag = zerokv::kTagAny - 1U;
constexpr unsigned kAlpsControlAmId = 0x4b56;  // "KV"

enum class AlpsControlType : uint16_t {
    kWriteRequest = 1001,
    kWriteGrant = 1002,
    kWriteDone = 1003,
    kWriteError = 1004,
    kWriteAbort = 1005,
};

using SteadyClock = std::chrono::steady_clock;

uint64_t elapsed_us(SteadyClock::time_point begin, SteadyClock::time_point end) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
}

uint64_t elapsed_us_nonzero(SteadyClock::time_point begin, SteadyClock::time_point end) {
    return std::max<uint64_t>(1, elapsed_us(begin, end));
}

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

std::vector<uint8_t> EncodeAmFrame(AlpsControlType type,
                                   uint64_t request_id,
                                   std::span<const uint8_t> payload) {
    std::vector<uint8_t> bytes;
    bytes.reserve((sizeof(uint64_t) * 2U) + payload.size());
    AppendU64(&bytes, static_cast<uint16_t>(type));
    AppendU64(&bytes, request_id);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

struct AmFrame {
    AlpsControlType type = AlpsControlType::kWriteError;
    uint64_t request_id = 0;
    std::span<const uint8_t> payload;
};

std::optional<AmFrame> DecodeAmFrame(std::span<const uint8_t> data) {
    size_t offset = 0;
    uint64_t wire_type = 0;
    uint64_t request_id = 0;
    if (!ReadU64(data, &offset, &wire_type) || !ReadU64(data, &offset, &request_id)) {
        return std::nullopt;
    }
    if (wire_type > std::numeric_limits<uint16_t>::max()) {
        return std::nullopt;
    }
    return AmFrame{
        .type = static_cast<AlpsControlType>(static_cast<uint16_t>(wire_type)),
        .request_id = request_id,
        .payload = data.subspan(offset),
    };
}

std::vector<uint8_t> EncodeError(const std::string& error) {
    return std::vector<uint8_t>(error.begin(), error.end());
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

zerokv::transport::Future<void> StartAmControlSend(ucp_worker_h worker,
                                                   ucp_ep_h endpoint,
                                                   AlpsControlType type,
                                                   uint64_t request_id,
                                                   std::span<const uint8_t> payload,
                                                   bool request_reply_endpoint) {
    if (worker == nullptr || endpoint == nullptr) {
        return zerokv::transport::Future<void>::make_error(
            zerokv::Status(zerokv::ErrorCode::kInvalidArgument, "missing UCX worker/endpoint"));
    }

    auto frame = std::make_shared<std::vector<uint8_t>>(
        EncodeAmFrame(type, request_id, payload));
    ucp_request_param_t params = {};
    params.op_attr_mask = UCP_OP_ATTR_FLAG_NO_IMM_CMPL | UCP_OP_ATTR_FIELD_FLAGS;
    params.flags = UCP_AM_SEND_FLAG_EAGER;
    if (request_reply_endpoint) {
        params.flags |= UCP_AM_SEND_FLAG_REPLY;
    }

    ucs_status_ptr_t status = ucp_am_send_nbx(endpoint,
                                              kAlpsControlAmId,
                                              nullptr,
                                              0,
                                              frame->data(),
                                              frame->size(),
                                              &params);
    if (status == nullptr) {
        return zerokv::transport::Future<void>::make_ready();
    }
    if (UCS_PTR_IS_ERR(status)) {
        const auto err = UCS_PTR_STATUS(status);
        return zerokv::transport::Future<void>::make_error(
            zerokv::Status(zerokv::ErrorCode::kTransportError,
                           std::string("ucp_am_send_nbx failed: ") + ucs_status_string(err)));
    }

    auto req = zerokv::transport::Request::create(
        status, worker, 0, std::shared_ptr<void>(frame, frame.get()));
    return zerokv::transport::Future<void>::make_request(std::move(req));
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

bool AlpsKvChannel::InstallAmHandler(const zerokv::transport::Worker::Ptr& worker,
                                     AmHandlerContext* context) {
    if (!worker || worker->native_handle() == nullptr || context == nullptr) {
        return false;
    }

    ucp_am_handler_param_t params = {};
    params.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID |
                        UCP_AM_HANDLER_PARAM_FIELD_FLAGS |
                        UCP_AM_HANDLER_PARAM_FIELD_CB |
                        UCP_AM_HANDLER_PARAM_FIELD_ARG;
    params.id = kAlpsControlAmId;
    params.flags = UCP_AM_FLAG_WHOLE_MSG;
    params.cb = &AlpsKvChannel::AmRecvCallback;
    params.arg = context;

    const auto status = ucp_worker_set_am_recv_handler(
        static_cast<ucp_worker_h>(worker->native_handle()), &params);
    if (status != UCS_OK) {
        std::cerr << "AlpsKvChannel: failed to install UCX AM handler: "
                  << ucs_status_string(status) << std::endl;
        return false;
    }
    return true;
}

ucs_status_t AlpsKvChannel::AmRecvCallback(void* arg,
                                           const void* header,
                                           size_t header_length,
                                           void* data,
                                           size_t length,
                                           const ucp_am_recv_param_t* param) {
    auto* context = static_cast<AmHandlerContext*>(arg);
    if (context == nullptr || context->channel == nullptr) {
        return UCS_ERR_INVALID_PARAM;
    }
    if (param != nullptr && (param->recv_attr & UCP_AM_RECV_ATTR_FLAG_RNDV) != 0) {
        return UCS_ERR_UNSUPPORTED;
    }

    const auto* bytes = static_cast<const uint8_t*>(data != nullptr ? data : header);
    const size_t bytes_length = (data != nullptr) ? length : header_length;
    context->channel->HandleAmMessage(
        context, std::span<const uint8_t>(bytes, bytes_length), param);
    return UCS_OK;
}

void AlpsKvChannel::HandleAmMessage(AmHandlerContext* context,
                                    std::span<const uint8_t> message,
                                    const ucp_am_recv_param_t* param) {
    if (context == nullptr) {
        return;
    }
    auto frame = DecodeAmFrame(message);
    if (!frame.has_value()) {
        return;
    }

    if (!context->server) {
        if (context->state == nullptr) {
            return;
        }
        if (frame->type == AlpsControlType::kWriteGrant) {
            auto grant = DecodeWriteGrant(frame->payload);
            if (!grant.has_value()) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(context->state->am_mutex);
                context->state->am_grants[frame->request_id] = std::move(*grant);
            }
            context->state->am_cv.notify_all();
            return;
        }
        if (frame->type == AlpsControlType::kWriteError) {
            {
                std::lock_guard<std::mutex> lock(context->state->am_mutex);
                context->state->am_errors[frame->request_id] = DecodeErrorPayload(frame->payload);
            }
            context->state->am_cv.notify_all();
        }
        return;
    }

    if (frame->type == AlpsControlType::kWriteRequest) {
        auto request = DecodeWriteRequest(frame->payload);
        if (!request.has_value()) {
            if (param != nullptr && (param->recv_attr & UCP_AM_RECV_ATTR_FIELD_REPLY_EP) != 0) {
                const auto error = EncodeError("failed to decode ALPS AM write request");
                QueueAmControlFrame(param->reply_ep,
                                    static_cast<uint16_t>(AlpsControlType::kWriteError),
                                    frame->request_id,
                                    error);
            }
            return;
        }
        if (param == nullptr || (param->recv_attr & UCP_AM_RECV_ATTR_FIELD_REPLY_EP) == 0 ||
            param->reply_ep == nullptr) {
            return;
        }

        std::shared_ptr<ReceiveSlot> slot;
        std::shared_ptr<BufferedMessage> buffered;
        std::string error;
        {
            std::lock_guard<std::mutex> lock(receive_slots_mutex_);
            auto slot_it = receive_slots_.find(request->message_tag);
            if (slot_it != receive_slots_.end() && slot_it->second &&
                !slot_it->second->reserved && !slot_it->second->done) {
                if (slot_it->second->size != request->size) {
                    error = "receive buffer size mismatch";
                } else {
                    slot_it->second->reserved = true;
                    direct_grant_ops_.fetch_add(1, std::memory_order_relaxed);
                    slot = slot_it->second;
                    pending_direct_reservations_[slot->reservation_id] = slot;
                }
            } else if (staged_messages_.find(request->message_tag) != staged_messages_.end()) {
                error = "duplicate outstanding ALPS message";
            }
        }

        if (error.empty() && !slot) {
            auto region = zerokv::transport::MemoryRegion::allocate(
                context_, static_cast<size_t>(request->size));
            if (!region) {
                error = "failed to allocate ALPS staging buffer";
            } else {
                buffered = std::make_shared<BufferedMessage>();
                buffered->region = std::move(region);
                buffered->remote_key = buffered->region->remote_key();
                buffered->message_tag = request->message_tag;
                buffered->size = static_cast<size_t>(request->size);
                buffered->reservation_id = next_reservation_id_.fetch_add(1);
                staged_grant_ops_.fetch_add(1, std::memory_order_relaxed);

                std::lock_guard<std::mutex> lock(receive_slots_mutex_);
                if (staged_messages_.find(request->message_tag) != staged_messages_.end()) {
                    error = "duplicate outstanding ALPS message";
                } else {
                    staged_messages_.emplace(request->message_tag, buffered);
                    pending_staged_reservations_[buffered->reservation_id] = buffered;
                }
            }
        }

        if (!error.empty()) {
            const auto payload = EncodeError(error);
            QueueAmControlFrame(param->reply_ep,
                                static_cast<uint16_t>(AlpsControlType::kWriteError),
                                frame->request_id,
                                payload);
            return;
        }

        WriteGrantPayload grant;
        grant.reservation_id = slot ? slot->reservation_id : buffered->reservation_id;
        grant.remote_addr = reinterpret_cast<uint64_t>(
            slot ? slot->region->address() : buffered->region->address());
        grant.rkey = slot ? slot->remote_key.data : buffered->remote_key.data;
        const auto grant_payload = EncodeWriteGrant(grant);
        if (!QueueAmControlFrame(param->reply_ep,
                                 static_cast<uint16_t>(AlpsControlType::kWriteGrant),
                                 frame->request_id,
                                 grant_payload)) {
            if (slot) {
                {
                    std::lock_guard<std::mutex> lock(receive_slots_mutex_);
                    pending_direct_reservations_.erase(slot->reservation_id);
                }
                FinishReceiveSlot(slot, "failed to send ALPS AM write grant");
            } else if (buffered) {
                std::lock_guard<std::mutex> lock(receive_slots_mutex_);
                staged_messages_.erase(buffered->message_tag);
                pending_staged_reservations_.erase(buffered->reservation_id);
            }
        }
        return;
    }

    if (frame->type == AlpsControlType::kWriteDone || frame->type == AlpsControlType::kWriteAbort) {
        const bool aborting = frame->type == AlpsControlType::kWriteAbort;
        auto done = DecodeWriteDone(frame->payload);
        if (!done.has_value()) {
            return;
        }

        std::shared_ptr<ReceiveSlot> slot;
        std::shared_ptr<BufferedMessage> buffered;
        zerokv::Tag staged_tag = 0;
        {
            std::lock_guard<std::mutex> lock(receive_slots_mutex_);
            auto direct_it = pending_direct_reservations_.find(done->reservation_id);
            if (direct_it != pending_direct_reservations_.end()) {
                slot = direct_it->second;
                pending_direct_reservations_.erase(direct_it);
            } else {
                auto staged_it = pending_staged_reservations_.find(done->reservation_id);
                if (staged_it != pending_staged_reservations_.end()) {
                    buffered = staged_it->second;
                    pending_staged_reservations_.erase(staged_it);
                    if (buffered) {
                        buffered->completed = true;
                        staged_tag = buffered->message_tag;
                    }
                }
            }
        }
        if (slot) {
            FinishReceiveSlot(slot, aborting ? "writer aborted ALPS AM transfer" : "");
        } else if (buffered) {
            if (aborting) {
                std::lock_guard<std::mutex> lock(receive_slots_mutex_);
                staged_messages_.erase(staged_tag);
            } else {
                TryDeliverBufferedMessage(staged_tag);
            }
        }
    }
}

bool AlpsKvChannel::QueueAmControlFrame(ucp_ep_h endpoint,
                                        uint16_t type,
                                        uint64_t request_id,
                                        std::span<const uint8_t> payload) {
    if (!recv_worker_ || recv_worker_->native_handle() == nullptr) {
        return false;
    }
    ReapAmSends();
    auto future = StartAmControlSend(static_cast<ucp_worker_h>(recv_worker_->native_handle()),
                                     endpoint,
                                     static_cast<AlpsControlType>(type),
                                     request_id,
                                     payload,
                                     false);
    if (!future.status().ok() && future.status().code() != zerokv::ErrorCode::kInProgress) {
        std::cerr << "AlpsKvChannel: failed to send UCX AM control frame: "
                  << future.status().message() << std::endl;
        return false;
    }
    if (!future.ready()) {
        std::lock_guard<std::mutex> lock(pending_am_sends_mutex_);
        pending_am_sends_.push_back(PendingAmSend{.future = std::move(future)});
    }
    return true;
}

void AlpsKvChannel::ReapAmSends() {
    std::lock_guard<std::mutex> lock(pending_am_sends_mutex_);
    pending_am_sends_.erase(
        std::remove_if(pending_am_sends_.begin(), pending_am_sends_.end(),
                       [](PendingAmSend& pending) {
                           return pending.future.ready();
                       }),
        pending_am_sends_.end());
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
    std::shared_ptr<RegisteredReceiveBuffer> cached;
    const BufferKey key{data, size};
    {
        std::lock_guard<std::mutex> lock(receive_slots_mutex_);
        auto cached_it = receive_cache_.find(key);
        if (cached_it != receive_cache_.end()) {
            cached = cached_it->second;
        }
    }
    if (!cached) {
        auto region = zerokv::transport::MemoryRegion::register_mem(
            context_, data, size, zerokv::MemoryType::kHost);
        if (!region) {
            std::cerr << "AlpsKvChannel: failed to register receive buffer." << std::endl;
            return nullptr;
        }
        auto registration = std::make_shared<RegisteredReceiveBuffer>();
        registration->region = std::move(region);
        registration->remote_key = registration->region->remote_key();
        {
            std::lock_guard<std::mutex> lock(receive_slots_mutex_);
            auto [it, inserted] = receive_cache_.emplace(key, registration);
            cached = it->second;
#ifdef ZEROKV_ALPS_TEST_HOOKS
            if (inserted) {
                receive_slot_register_ops_.fetch_add(1, std::memory_order_relaxed);
            }
#endif
        }
    }

    auto slot = std::make_shared<ReceiveSlot>();
    slot->region = cached->region;
    slot->remote_key = cached->remote_key;
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
    if (slot) {
        pending_direct_reservations_.erase(slot->reservation_id);
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

    const auto copy_begin = SteadyClock::now();
    std::memcpy(slot->region->address(), buffered->region->address(), buffered->size);
    staged_delivery_ops_.fetch_add(1, std::memory_order_relaxed);
    staged_copy_bytes_.fetch_add(buffered->size, std::memory_order_relaxed);
    staged_copy_us_.fetch_add(elapsed_us(copy_begin, SteadyClock::now()),
                              std::memory_order_relaxed);
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
                direct_grant_ops_.fetch_add(1, std::memory_order_relaxed);
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
            staged_grant_ops_.fetch_add(1, std::memory_order_relaxed);

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

    listen_am_context_ = std::make_unique<AmHandlerContext>();
    listen_am_context_->channel = this;
    listen_am_context_->server = true;
    if (!InstallAmHandler(recv_worker_, listen_am_context_.get())) {
        recv_worker_.reset();
        context_.reset();
        return false;
    }

    running_ = true;
    mode_ = Mode::kListen;
    recv_worker_->start_progress_thread();

    listener_ = recv_worker_->listen(bind_address, [this](zerokv::transport::Endpoint::Ptr ep) {
        if (!ep) {
            return;
        }
        ReapAmSends();
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

    auto state = std::make_shared<PerThreadState>();
    state->worker = zerokv::transport::Worker::create(context_);
    if (!state->worker) {
        std::cerr << "AlpsKvChannel: failed to create worker for thread." << std::endl;
        return nullptr;
    }
    state->am_context.channel = this;
    state->am_context.state = state.get();
    state->am_context.server = false;
    if (!InstallAmHandler(state->worker, &state->am_context)) {
        return nullptr;
    }
    state->worker->start_progress_thread();

    auto future = state->worker->connect(remote_address_);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(connect_timeout_ms_);

    if (!SpinUntilReady(future, deadline)) {
        std::cerr << "AlpsKvChannel: timed out connecting to " << remote_address_ << std::endl;
        state->worker->stop_progress_thread();
        return nullptr;
    }
    auto conn_result = future.get();
    if (!future.status().ok() || conn_result == nullptr) {
        std::cerr << "AlpsKvChannel: connect failed: " << future.status().message() << std::endl;
        state->worker->stop_progress_thread();
        return nullptr;
    }

    state->endpoint = std::move(conn_result);

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
    {
        std::lock_guard<std::mutex> lock(per_thread_mutex_);
        for (const auto& [tid, state] : per_thread_states_) {
            if (state) {
                state->am_cv.notify_all();
            }
        }
    }

    std::vector<std::shared_ptr<ReceiveSlot>> slots;
    {
        std::lock_guard<std::mutex> lock(receive_slots_mutex_);
        for (const auto& [tag, slot] : receive_slots_) {
            slots.push_back(slot);
        }
        receive_slots_.clear();
        receive_cache_.clear();
        staged_messages_.clear();
        pending_direct_reservations_.clear();
        pending_staged_reservations_.clear();
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
        {
            std::lock_guard<std::mutex> lock(pending_am_sends_mutex_);
            pending_am_sends_.clear();
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
        listen_am_context_.reset();
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

    const auto message_tag = MakeMessageTag(tag, index, src, dst);
    const auto request_id = state->next_request_id++;
    const auto write_request = EncodeWriteRequest(WriteRequestPayload{
        .message_tag = message_tag,
        .size = size,
    });
    const auto deadline =
        SteadyClock::now() + std::chrono::milliseconds(connect_timeout_ms_);
    const auto control_request_begin = SteadyClock::now();
    auto request_send = StartAmControlSend(
        static_cast<ucp_worker_h>(state->worker->native_handle()),
        static_cast<ucp_ep_h>(state->endpoint->native_handle()),
        AlpsControlType::kWriteRequest,
        request_id,
        write_request,
        true);
    if (!request_send.status().ok() &&
        request_send.status().code() != zerokv::ErrorCode::kInProgress) {
        std::cerr << "AlpsKvChannel::WriteBytes: failed to send AM write request: "
                  << request_send.status().message() << std::endl;
        return false;
    }
    auto send_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - SteadyClock::now());
    if (send_timeout.count() <= 0) {
        send_timeout = std::chrono::milliseconds(1);
    }
    if (!request_send.ready() && !request_send.get(send_timeout)) {
        std::cerr << "AlpsKvChannel::WriteBytes: failed to complete AM write request send: "
                  << request_send.status().message() << std::endl;
        return false;
    }

    std::optional<WriteGrantPayload> grant;
    std::string control_error;
    {
        std::unique_lock<std::mutex> lock(state->am_mutex);
        const bool ready = state->am_cv.wait_until(lock, deadline, [&]() {
            return !running_ ||
                   state->am_grants.find(request_id) != state->am_grants.end() ||
                   state->am_errors.find(request_id) != state->am_errors.end();
        });
        if (!ready) {
            std::cerr << "AlpsKvChannel::WriteBytes: timed out waiting for AM write grant."
                      << std::endl;
            return false;
        }
        auto error_it = state->am_errors.find(request_id);
        if (error_it != state->am_errors.end()) {
            control_error = std::move(error_it->second);
            state->am_errors.erase(error_it);
        } else {
            auto grant_it = state->am_grants.find(request_id);
            if (grant_it != state->am_grants.end()) {
                grant = std::move(grant_it->second);
                state->am_grants.erase(grant_it);
            }
        }
    }
    if (!control_error.empty()) {
        std::cerr << "AlpsKvChannel::WriteBytes: AM control error: "
                  << control_error << std::endl;
        return false;
    }
    if (!grant.has_value()) {
        std::cerr << "AlpsKvChannel::WriteBytes: channel stopped before AM write grant."
                  << std::endl;
        return false;
    }
    control_request_grant_us_.fetch_add(
        elapsed_us_nonzero(control_request_begin, SteadyClock::now()), std::memory_order_relaxed);

    auto region = GetOrRegisterSendRegion(state->send_cache, data, size);
    if (!region) {
        std::cerr << "AlpsKvChannel::WriteBytes: failed to register send buffer." << std::endl;
        return false;
    }

    zerokv::transport::RemoteKey remote_key;
    remote_key.data = grant->rkey;
    auto send_abort = [&]() {
        const auto abort_payload = EncodeWriteDone(WriteDonePayload{
            .reservation_id = grant->reservation_id,
        });
        auto abort_send = StartAmControlSend(
            static_cast<ucp_worker_h>(state->worker->native_handle()),
            static_cast<ucp_ep_h>(state->endpoint->native_handle()),
            AlpsControlType::kWriteAbort,
            request_id,
            abort_payload,
            false);
        if (abort_send.status().ok() ||
            abort_send.status().code() == zerokv::ErrorCode::kInProgress) {
            SpinUntilReady(abort_send, SteadyClock::now() + std::chrono::milliseconds(200));
        }
    };
#ifdef ZEROKV_ALPS_TEST_HOOKS
    const std::string rkey_cache_key(
        reinterpret_cast<const char*>(grant->rkey.data()), grant->rkey.size());
    if (state->remote_rkey_cache.insert(rkey_cache_key).second) {
        remote_rkey_unpack_ops_.fetch_add(1, std::memory_order_relaxed);
    }
#endif
    const auto put_begin = SteadyClock::now();
    auto put = state->endpoint->put(region, 0, grant->remote_addr, remote_key, size);
    SpinUntilReady(put);
    if (!put.status().ok()) {
        send_abort();
        std::cerr << "AlpsKvChannel::WriteBytes: put failed: " << put.status().message()
                  << std::endl;
        return false;
    }
    rdma_put_us_.fetch_add(elapsed_us(put_begin, SteadyClock::now()), std::memory_order_relaxed);

    const auto flush_begin = SteadyClock::now();
    auto flush = state->endpoint->flush();
    SpinUntilReady(flush);
    if (!flush.status().ok()) {
        send_abort();
        std::cerr << "AlpsKvChannel::WriteBytes: flush failed: " << flush.status().message()
                  << std::endl;
        return false;
    }
    flush_us_.fetch_add(elapsed_us(flush_begin, SteadyClock::now()), std::memory_order_relaxed);

    const auto write_done = EncodeWriteDone(WriteDonePayload{
        .reservation_id = grant->reservation_id,
    });
    const auto write_done_begin = SteadyClock::now();
    auto done_send = StartAmControlSend(
        static_cast<ucp_worker_h>(state->worker->native_handle()),
        static_cast<ucp_ep_h>(state->endpoint->native_handle()),
        AlpsControlType::kWriteDone,
        request_id,
        write_done,
        false);
    if (!done_send.status().ok() &&
        done_send.status().code() != zerokv::ErrorCode::kInProgress) {
        std::cerr << "AlpsKvChannel::WriteBytes: failed to send AM write completion: "
                  << done_send.status().message() << std::endl;
        return false;
    }
    SpinUntilReady(done_send);
    write_done_us_.fetch_add(
        elapsed_us_nonzero(write_done_begin, SteadyClock::now()), std::memory_order_relaxed);
    write_ops_.fetch_add(1, std::memory_order_relaxed);

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

bool AlpsKvChannel::WaitForReceiveSlots(size_t expected, int timeout_ms) {
    if (expected == 0) {
        return true;
    }
    if (timeout_ms < 0) {
        return false;
    }

    std::unique_lock<std::mutex> lock(receive_slots_mutex_);
    return receive_slots_cv_.wait_for(
        lock, std::chrono::milliseconds(timeout_ms), [this, expected]() {
            return receive_slots_.size() >= expected;
        });
}

// ============================================================================
// Accessors

std::string AlpsKvChannel::local_address() const {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    return local_address_;
}

AlpsKvChannel::WriteTimingStats AlpsKvChannel::write_timing_stats() const {
    return WriteTimingStats{
        .write_ops = write_ops_.load(std::memory_order_relaxed),
        .control_request_grant_us = control_request_grant_us_.load(std::memory_order_relaxed),
        .rdma_put_us = rdma_put_us_.load(std::memory_order_relaxed),
        .flush_us = flush_us_.load(std::memory_order_relaxed),
        .write_done_us = write_done_us_.load(std::memory_order_relaxed),
    };
}

void AlpsKvChannel::reset_write_timing_stats() {
    write_ops_.store(0, std::memory_order_relaxed);
    control_request_grant_us_.store(0, std::memory_order_relaxed);
    rdma_put_us_.store(0, std::memory_order_relaxed);
    flush_us_.store(0, std::memory_order_relaxed);
    write_done_us_.store(0, std::memory_order_relaxed);
}

AlpsKvChannel::ReceivePathStats AlpsKvChannel::receive_path_stats() const {
    return ReceivePathStats{
        .direct_grant_ops = direct_grant_ops_.load(std::memory_order_relaxed),
        .staged_grant_ops = staged_grant_ops_.load(std::memory_order_relaxed),
        .staged_delivery_ops = staged_delivery_ops_.load(std::memory_order_relaxed),
        .staged_copy_bytes = staged_copy_bytes_.load(std::memory_order_relaxed),
        .staged_copy_us = staged_copy_us_.load(std::memory_order_relaxed),
    };
}

void AlpsKvChannel::reset_receive_path_stats() {
    direct_grant_ops_.store(0, std::memory_order_relaxed);
    staged_grant_ops_.store(0, std::memory_order_relaxed);
    staged_delivery_ops_.store(0, std::memory_order_relaxed);
    staged_copy_bytes_.store(0, std::memory_order_relaxed);
    staged_copy_us_.store(0, std::memory_order_relaxed);
}

#ifdef ZEROKV_ALPS_TEST_HOOKS
AlpsKvChannel::DebugStats AlpsKvChannel::debug_stats() const {
    return DebugStats{
        .payload_tag_send_ops = payload_tag_send_ops_.load(),
        .rma_put_ops = rma_put_ops_.load(),
        .receive_slot_register_ops = receive_slot_register_ops_.load(),
        .remote_rkey_unpack_ops = remote_rkey_unpack_ops_.load(),
    };
}
#endif

}  // namespace zerokv::compat
