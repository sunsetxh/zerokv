#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace axon::kv::detail {

constexpr uint32_t kProtocolMagic = 0x4E4F5841;  // "AXON" in little-endian memory order.
constexpr uint32_t kMaxFieldSize = 16 * 1024 * 1024;
constexpr size_t kHeaderWireSize = 20;

enum class MsgType : uint16_t {
    kRegisterNode = 1,
    kRegisterNodeResp = 2,
    kPutMeta = 3,
    kPutMetaResp = 4,
    kGetMeta = 5,
    kGetMetaResp = 6,
    kUnpublish = 7,
    kHeartbeat = 8,
    kUnpublishResp = 9,
    kError = 255,
};

enum class MsgStatus : uint16_t {
    kOk = 0,
    kNotFound = 1,
    kAlreadyExists = 2,
    kInvalidRequest = 3,
    kStaleOwner = 4,
    kInternalError = 5,
};

struct MsgHeader {
    uint32_t magic = kProtocolMagic;
    uint16_t type = 0;
    uint16_t flags = 0;  // Reserved for future use.
    uint32_t payload_length = 0;  // Set automatically by encode_message().
    uint64_t request_id = 0;
};

struct KeyMetadata {
    std::string key;
    std::string owner_node_id;
    std::string owner_data_addr;
    uint64_t remote_addr = 0;
    std::vector<uint8_t> rkey;
    size_t size = 0;
    uint64_t version = 0;
};

struct RegisterNodeRequest {
    std::string node_id;
    std::string control_addr;
    std::string data_addr;
};

struct RegisterNodeResponse {
    MsgStatus status = MsgStatus::kOk;
    std::string assigned_node_id;
    std::string message;
};

struct PutMetaRequest {
    KeyMetadata metadata;
};

struct PutMetaResponse {
    MsgStatus status = MsgStatus::kOk;
    std::string message;
};

struct GetMetaRequest {
    std::string key;
};

struct GetMetaResponse {
    MsgStatus status = MsgStatus::kOk;
    std::optional<KeyMetadata> metadata;
    std::string message;
};

struct UnpublishRequest {
    std::string key;
    std::string owner_node_id;
};

struct UnpublishResponse {
    MsgStatus status = MsgStatus::kOk;
    std::string message;
};

struct HeartbeatRequest {
    std::string node_id;
    uint64_t timestamp_ms = 0;
};

struct ErrorResponse {
    MsgStatus status = MsgStatus::kInternalError;
    std::string message;
};

[[nodiscard]] std::vector<uint8_t> encode_header(const MsgHeader& header);
[[nodiscard]] std::optional<MsgHeader> decode_header(std::span<const uint8_t> data);
[[nodiscard]] std::vector<uint8_t> encode_message(const MsgHeader& header, std::span<const uint8_t> payload);
[[nodiscard]] std::optional<std::pair<MsgHeader, std::span<const uint8_t>>>
decode_message(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const RegisterNodeRequest& msg);
[[nodiscard]] std::optional<RegisterNodeRequest> decode_register_node_request(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const RegisterNodeResponse& msg);
[[nodiscard]] std::optional<RegisterNodeResponse> decode_register_node_response(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const PutMetaRequest& msg);
[[nodiscard]] std::optional<PutMetaRequest> decode_put_meta_request(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const PutMetaResponse& msg);
[[nodiscard]] std::optional<PutMetaResponse> decode_put_meta_response(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const GetMetaRequest& msg);
[[nodiscard]] std::optional<GetMetaRequest> decode_get_meta_request(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const GetMetaResponse& msg);
[[nodiscard]] std::optional<GetMetaResponse> decode_get_meta_response(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const UnpublishRequest& msg);
[[nodiscard]] std::optional<UnpublishRequest> decode_unpublish_request(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const UnpublishResponse& msg);
[[nodiscard]] std::optional<UnpublishResponse> decode_unpublish_response(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const HeartbeatRequest& msg);
[[nodiscard]] std::optional<HeartbeatRequest> decode_heartbeat_request(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const ErrorResponse& msg);
[[nodiscard]] std::optional<ErrorResponse> decode_error_response(std::span<const uint8_t> data);

}  // namespace axon::kv::detail
