#include "kv/protocol.h"

#include <cstring>
#include <limits>

namespace axon::kv::detail {

namespace {

class Encoder {
public:
    void u16(uint16_t value) { pod(value); }
    void u32(uint32_t value) { pod(value); }
    void u64(uint64_t value) { pod(value); }

    void str(const std::string& value) {
        u32(static_cast<uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void blob(std::span<const uint8_t> value) {
        u32(static_cast<uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    [[nodiscard]] std::vector<uint8_t> finish() && { return std::move(bytes_); }

private:
    template <typename T>
    void pod(T value) {
        const auto* ptr = reinterpret_cast<const uint8_t*>(&value);
        bytes_.insert(bytes_.end(), ptr, ptr + sizeof(T));
    }

    std::vector<uint8_t> bytes_;
};

class Decoder {
public:
    explicit Decoder(std::span<const uint8_t> data) : data_(data) {}

    [[nodiscard]] bool u16(uint16_t& value) { return pod(value); }
    [[nodiscard]] bool u32(uint32_t& value) { return pod(value); }
    [[nodiscard]] bool u64(uint64_t& value) { return pod(value); }

    [[nodiscard]] bool str(std::string& value) {
        std::vector<uint8_t> bytes;
        if (!blob(bytes)) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    }

    [[nodiscard]] bool blob(std::vector<uint8_t>& value) {
        uint32_t size = 0;
        if (!u32(size) || size > kMaxFieldSize || size > remaining()) {
            return false;
        }
        value.assign(data_.begin() + offset_, data_.begin() + offset_ + size);
        offset_ += size;
        return true;
    }

    [[nodiscard]] bool done() const noexcept { return offset_ == data_.size(); }
    [[nodiscard]] size_t remaining() const noexcept { return data_.size() - offset_; }

private:
    template <typename T>
    [[nodiscard]] bool pod(T& value) {
        if (remaining() < sizeof(T)) {
            return false;
        }
        std::memcpy(&value, data_.data() + offset_, sizeof(T));
        offset_ += sizeof(T);
        return true;
    }

    std::span<const uint8_t> data_;
    size_t offset_ = 0;
};

void encode_metadata(Encoder& enc, const KeyMetadata& metadata) {
    enc.str(metadata.key);
    enc.str(metadata.owner_node_id);
    enc.str(metadata.owner_data_addr);
    enc.u64(metadata.remote_addr);
    enc.blob(metadata.rkey);
    enc.u64(static_cast<uint64_t>(metadata.size));
    enc.u64(metadata.version);
}

bool decode_metadata(Decoder& dec, KeyMetadata& metadata) {
    uint64_t size = 0;
    return dec.str(metadata.key) &&
           dec.str(metadata.owner_node_id) &&
           dec.str(metadata.owner_data_addr) &&
           dec.u64(metadata.remote_addr) &&
           dec.blob(metadata.rkey) &&
           dec.u64(size) &&
           dec.u64(metadata.version) &&
           (metadata.size = static_cast<size_t>(size), true);
}

template <typename T>
std::optional<T> decode_checked(std::span<const uint8_t> data, bool (*fn)(Decoder&, T&)) {
    Decoder dec(data);
    T value;
    if (!fn(dec, value) || !dec.done()) {
        return std::nullopt;
    }
    return value;
}

bool decode_register_node_request_impl(Decoder& dec, RegisterNodeRequest& msg) {
    return dec.str(msg.node_id) &&
           dec.str(msg.control_addr) &&
           dec.str(msg.data_addr);
}

bool decode_register_node_response_impl(Decoder& dec, RegisterNodeResponse& msg) {
    uint16_t status = 0;
    return dec.u16(status) &&
           dec.str(msg.assigned_node_id) &&
           dec.str(msg.message) &&
           (msg.status = static_cast<MsgStatus>(status), true);
}

bool decode_put_meta_request_impl(Decoder& dec, PutMetaRequest& msg) {
    return decode_metadata(dec, msg.metadata);
}

bool decode_put_meta_response_impl(Decoder& dec, PutMetaResponse& msg) {
    uint16_t status = 0;
    return dec.u16(status) &&
           dec.str(msg.message) &&
           (msg.status = static_cast<MsgStatus>(status), true);
}

bool decode_get_meta_request_impl(Decoder& dec, GetMetaRequest& msg) {
    return dec.str(msg.key);
}

bool decode_get_meta_response_impl(Decoder& dec, GetMetaResponse& msg) {
    uint16_t status = 0;
    uint16_t has_metadata = 0;
    if (!dec.u16(status) || !dec.u16(has_metadata)) {
        return false;
    }
    msg.status = static_cast<MsgStatus>(status);
    if (has_metadata != 0) {
        KeyMetadata metadata;
        if (!decode_metadata(dec, metadata)) {
            return false;
        }
        msg.metadata = std::move(metadata);
    }
    return dec.str(msg.message);
}

bool decode_unpublish_request_impl(Decoder& dec, UnpublishRequest& msg) {
    return dec.str(msg.key) &&
           dec.str(msg.owner_node_id);
}

bool decode_unpublish_response_impl(Decoder& dec, UnpublishResponse& msg) {
    uint16_t status = 0;
    return dec.u16(status) &&
           dec.str(msg.message) &&
           (msg.status = static_cast<MsgStatus>(status), true);
}

bool decode_heartbeat_request_impl(Decoder& dec, HeartbeatRequest& msg) {
    return dec.str(msg.node_id) &&
           dec.u64(msg.timestamp_ms);
}

bool decode_error_response_impl(Decoder& dec, ErrorResponse& msg) {
    uint16_t status = 0;
    return dec.u16(status) &&
           dec.str(msg.message) &&
           (msg.status = static_cast<MsgStatus>(status), true);
}

}  // namespace

std::vector<uint8_t> encode_header(const MsgHeader& header) {
    Encoder enc;
    enc.u32(header.magic);
    enc.u16(header.type);
    enc.u16(header.flags);
    enc.u32(header.payload_length);
    enc.u64(header.request_id);
    return std::move(enc).finish();
}

std::optional<MsgHeader> decode_header(std::span<const uint8_t> data) {
    Decoder dec(data);
    MsgHeader header;
    if (!dec.u32(header.magic) ||
        !dec.u16(header.type) ||
        !dec.u16(header.flags) ||
        !dec.u32(header.payload_length) ||
        !dec.u64(header.request_id)) {
        return std::nullopt;
    }
    if (header.magic != kProtocolMagic) {
        return std::nullopt;
    }
    return header;
}

std::vector<uint8_t> encode_message(const MsgHeader& header, std::span<const uint8_t> payload) {
    MsgHeader actual = header;
    actual.payload_length = static_cast<uint32_t>(payload.size());
    auto bytes = encode_header(actual);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::optional<std::pair<MsgHeader, std::span<const uint8_t>>>
decode_message(std::span<const uint8_t> data) {
    static_assert(kHeaderWireSize == 20, "Header wire size must remain 20 bytes");
    if (data.size() < kHeaderWireSize) {
        return std::nullopt;
    }
    auto header = decode_header(data.first(kHeaderWireSize));
    if (!header.has_value()) {
        return std::nullopt;
    }
    const size_t total = kHeaderWireSize + header->payload_length;
    if (data.size() < total) {
        return std::nullopt;
    }
    return std::make_pair(*header, data.subspan(kHeaderWireSize, header->payload_length));
}

std::vector<uint8_t> encode(const RegisterNodeRequest& msg) {
    Encoder enc;
    enc.str(msg.node_id);
    enc.str(msg.control_addr);
    enc.str(msg.data_addr);
    return std::move(enc).finish();
}

std::optional<RegisterNodeRequest> decode_register_node_request(std::span<const uint8_t> data) {
    return decode_checked<RegisterNodeRequest>(data, decode_register_node_request_impl);
}

std::vector<uint8_t> encode(const RegisterNodeResponse& msg) {
    Encoder enc;
    enc.u16(static_cast<uint16_t>(msg.status));
    enc.str(msg.assigned_node_id);
    enc.str(msg.message);
    return std::move(enc).finish();
}

std::optional<RegisterNodeResponse> decode_register_node_response(std::span<const uint8_t> data) {
    return decode_checked<RegisterNodeResponse>(data, decode_register_node_response_impl);
}

std::vector<uint8_t> encode(const PutMetaRequest& msg) {
    Encoder enc;
    encode_metadata(enc, msg.metadata);
    return std::move(enc).finish();
}

std::optional<PutMetaRequest> decode_put_meta_request(std::span<const uint8_t> data) {
    return decode_checked<PutMetaRequest>(data, decode_put_meta_request_impl);
}

std::vector<uint8_t> encode(const PutMetaResponse& msg) {
    Encoder enc;
    enc.u16(static_cast<uint16_t>(msg.status));
    enc.str(msg.message);
    return std::move(enc).finish();
}

std::optional<PutMetaResponse> decode_put_meta_response(std::span<const uint8_t> data) {
    return decode_checked<PutMetaResponse>(data, decode_put_meta_response_impl);
}

std::vector<uint8_t> encode(const GetMetaRequest& msg) {
    Encoder enc;
    enc.str(msg.key);
    return std::move(enc).finish();
}

std::optional<GetMetaRequest> decode_get_meta_request(std::span<const uint8_t> data) {
    return decode_checked<GetMetaRequest>(data, decode_get_meta_request_impl);
}

std::vector<uint8_t> encode(const GetMetaResponse& msg) {
    Encoder enc;
    enc.u16(static_cast<uint16_t>(msg.status));
    enc.u16(msg.metadata.has_value() ? 1 : 0);
    if (msg.metadata.has_value()) {
        encode_metadata(enc, *msg.metadata);
    }
    enc.str(msg.message);
    return std::move(enc).finish();
}

std::optional<GetMetaResponse> decode_get_meta_response(std::span<const uint8_t> data) {
    return decode_checked<GetMetaResponse>(data, decode_get_meta_response_impl);
}

std::vector<uint8_t> encode(const UnpublishRequest& msg) {
    Encoder enc;
    enc.str(msg.key);
    enc.str(msg.owner_node_id);
    return std::move(enc).finish();
}

std::optional<UnpublishRequest> decode_unpublish_request(std::span<const uint8_t> data) {
    return decode_checked<UnpublishRequest>(data, decode_unpublish_request_impl);
}

std::vector<uint8_t> encode(const UnpublishResponse& msg) {
    Encoder enc;
    enc.u16(static_cast<uint16_t>(msg.status));
    enc.str(msg.message);
    return std::move(enc).finish();
}

std::optional<UnpublishResponse> decode_unpublish_response(std::span<const uint8_t> data) {
    return decode_checked<UnpublishResponse>(data, decode_unpublish_response_impl);
}

std::vector<uint8_t> encode(const HeartbeatRequest& msg) {
    Encoder enc;
    enc.str(msg.node_id);
    enc.u64(msg.timestamp_ms);
    return std::move(enc).finish();
}

std::optional<HeartbeatRequest> decode_heartbeat_request(std::span<const uint8_t> data) {
    return decode_checked<HeartbeatRequest>(data, decode_heartbeat_request_impl);
}

std::vector<uint8_t> encode(const ErrorResponse& msg) {
    Encoder enc;
    enc.u16(static_cast<uint16_t>(msg.status));
    enc.str(msg.message);
    return std::move(enc).finish();
}

std::optional<ErrorResponse> decode_error_response(std::span<const uint8_t> data) {
    return decode_checked<ErrorResponse>(data, decode_error_response_impl);
}

}  // namespace axon::kv::detail
