#ifndef ZEROKV_PROTOCOL_H
#define ZEROKV_PROTOCOL_H

#include "common.h"
#include <string>
#include <vector>

namespace zerokv {

// Request header (16 bytes)
struct RequestHeader {
    uint8_t  opcode;      // OpCode
    uint8_t  flags;
    uint16_t key_len;
    uint32_t value_len;
    uint64_t request_id;
    uint32_t user_rkey;   // For user memory operations
    uint64_t user_addr;   // For user memory operations

    static constexpr size_t SIZE = 16;
};

// Response header (16 bytes)
struct ResponseHeader {
    uint8_t  status;      // Status
    uint8_t  flags;
    uint16_t reserved;
    uint32_t value_len;
    uint64_t request_id;

    static constexpr size_t SIZE = 16;
};

// Protocol codec
class ProtocolCodec {
public:
    // Encode request
    static std::vector<uint8_t> encode_request(
        OpCode opcode,
        const std::string& key,
        const void* value,
        size_t value_len,
        uint64_t request_id,
        void* user_addr = nullptr,
        uint32_t user_rkey = 0);

    // Decode request
    static bool decode_request(
        const std::vector<uint8_t>& data,
        RequestHeader& header,
        std::string& key,
        void*& value);

    // Encode response
    static std::vector<uint8_t> encode_response(
        Status status,
        uint64_t request_id,
        const void* value = nullptr,
        size_t value_len = 0);

    // Decode response
    static bool decode_response(
        const std::vector<uint8_t>& data,
        ResponseHeader& header,
        void*& value,
        size_t& value_len);
};

} // namespace zerokv

#endif // ZEROKV_PROTOCOL_H
