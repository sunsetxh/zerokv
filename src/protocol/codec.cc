#include "zerokv/protocol.h"
#include <cstring>
#include <algorithm>

namespace zerokv {

// Encode request
std::vector<uint8_t> ProtocolCodec::encode_request(
    OpCode opcode,
    const std::string& key,
    const void* value,
    size_t value_len,
    uint64_t request_id,
    void* user_addr,
    uint32_t user_rkey) {

    std::vector<uint8_t> data;
    data.reserve(RequestHeader::SIZE + key.size() + value_len);

    // Header
    RequestHeader header;
    header.opcode = static_cast<uint8_t>(opcode);
    header.flags = 0;
    header.key_len = static_cast<uint16_t>(key.size());
    header.value_len = static_cast<uint32_t>(value_len);
    header.request_id = request_id;
    header.user_rkey = user_rkey;
    header.user_addr = reinterpret_cast<uint64_t>(user_addr);

    data.resize(RequestHeader::SIZE);
    std::memcpy(data.data(), &header, RequestHeader::SIZE);

    // Key
    data.insert(data.end(), key.begin(), key.end());

    // Value (if provided)
    if (value && value_len > 0) {
        const uint8_t* value_bytes = static_cast<const uint8_t*>(value);
        data.insert(data.end(), value_bytes, value_bytes + value_len);
    }

    return data;
}

// Decode request
bool ProtocolCodec::decode_request(
    const std::vector<uint8_t>& data,
    RequestHeader& header,
    std::string& key,
    void*& value) {

    if (data.size() < RequestHeader::SIZE) {
        return false;
    }

    // Header
    std::memcpy(&header, data.data(), RequestHeader::SIZE);

    // Validate
    size_t expected_size = RequestHeader::SIZE + header.key_len + header.value_len;
    if (data.size() < expected_size) {
        return false;
    }

    // Key
    key.assign(reinterpret_cast<const char*>(data.data() + RequestHeader::SIZE),
               header.key_len);

    // Value
    if (header.value_len > 0) {
        value = const_cast<uint8_t*>(data.data() + RequestHeader::SIZE + header.key_len);
    } else {
        value = nullptr;
    }

    return true;
}

// Encode response
std::vector<uint8_t> ProtocolCodec::encode_response(
    Status status,
    uint64_t request_id,
    const void* value,
    size_t value_len) {

    std::vector<uint8_t> data;
    data.reserve(ResponseHeader::SIZE + value_len);

    // Header
    ResponseHeader header;
    header.status = static_cast<uint8_t>(status);
    header.flags = 0;
    header.reserved = 0;
    header.value_len = static_cast<uint32_t>(value_len);
    header.request_id = request_id;

    data.resize(ResponseHeader::SIZE);
    std::memcpy(data.data(), &header, ResponseHeader::SIZE);

    // Value
    if (value && value_len > 0) {
        const uint8_t* value_bytes = static_cast<const uint8_t*>(value);
        data.insert(data.end(), value_bytes, value_bytes + value_len);
    }

    return data;
}

// Decode response
bool ProtocolCodec::decode_response(
    const std::vector<uint8_t>& data,
    ResponseHeader& header,
    void*& value,
    size_t& value_len) {

    if (data.size() < ResponseHeader::SIZE) {
        return false;
    }

    // Header
    std::memcpy(&header, data.data(), ResponseHeader::SIZE);

    // Value
    value_len = header.value_len;
    if (header.value_len > 0) {
        value = const_cast<uint8_t*>(data.data() + ResponseHeader::SIZE);
    } else {
        value = nullptr;
    }

    return true;
}

} // namespace zerokv
