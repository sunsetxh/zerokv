#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace zerokv::core::detail {

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
    kGetPushTarget = 10,
    kGetPushTargetResp = 11,
    kPushCommit = 12,
    kPushCommitResp = 13,
    kSubscribe = 14,
    kSubscribeResp = 15,
    kUnsubscribe = 16,
    kUnsubscribeResp = 17,
    kSubscriptionEvent = 18,
    kReservePushInbox = 19,
    kReservePushInboxResp = 20,
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
    std::string push_control_addr;
    std::string subscription_control_addr;
    uint64_t push_inbox_remote_addr = 0;
    std::vector<uint8_t> push_inbox_rkey;
    uint64_t push_inbox_capacity = 0;
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

struct GetPushTargetRequest {
    std::string target_node_id;
};

struct GetPushTargetResponse {
    MsgStatus status = MsgStatus::kOk;
    std::string target_node_id;
    std::string target_data_addr;
    std::string push_control_addr;
    uint64_t push_inbox_remote_addr = 0;
    std::vector<uint8_t> push_inbox_rkey;
    uint64_t push_inbox_capacity = 0;
    std::string message;
};

struct ReservePushInboxRequest {
    std::string target_node_id;
    std::string sender_node_id;
    std::string key;
    uint64_t value_size = 0;
};

struct ReservePushInboxResponse {
    MsgStatus status = MsgStatus::kOk;
    std::string message;
};

struct PushCommitRequest {
    std::string target_node_id;
    std::string sender_node_id;
    std::string key;
    uint64_t value_size = 0;
};

struct PushCommitResponse {
    MsgStatus status = MsgStatus::kOk;
    std::string message;
};

enum class SubscriptionEventType : uint16_t {
    kPublished = 1,
    kUpdated = 2,
    kUnpublished = 3,
    kOwnerLost = 4,
};

struct SubscribeRequest {
    std::string subscriber_node_id;
    std::string key;
};

struct SubscribeResponse {
    MsgStatus status = MsgStatus::kOk;
    std::string message;
};

struct UnsubscribeRequest {
    std::string subscriber_node_id;
    std::string key;
};

struct UnsubscribeResponse {
    MsgStatus status = MsgStatus::kOk;
    std::string message;
};

struct SubscriptionEvent {
    SubscriptionEventType type = SubscriptionEventType::kPublished;
    std::string key;
    std::string owner_node_id;
    uint64_t version = 0;
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

[[nodiscard]] std::vector<uint8_t> encode(const GetPushTargetRequest& msg);
[[nodiscard]] std::optional<GetPushTargetRequest> decode_get_push_target_request(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const GetPushTargetResponse& msg);
[[nodiscard]] std::optional<GetPushTargetResponse> decode_get_push_target_response(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const ReservePushInboxRequest& msg);
[[nodiscard]] std::optional<ReservePushInboxRequest> decode_reserve_push_inbox_request(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const ReservePushInboxResponse& msg);
[[nodiscard]] std::optional<ReservePushInboxResponse> decode_reserve_push_inbox_response(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const PushCommitRequest& msg);
[[nodiscard]] std::optional<PushCommitRequest> decode_push_commit_request(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const PushCommitResponse& msg);
[[nodiscard]] std::optional<PushCommitResponse> decode_push_commit_response(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const SubscribeRequest& msg);
[[nodiscard]] std::optional<SubscribeRequest> decode_subscribe_request(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const SubscribeResponse& msg);
[[nodiscard]] std::optional<SubscribeResponse> decode_subscribe_response(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const UnsubscribeRequest& msg);
[[nodiscard]] std::optional<UnsubscribeRequest> decode_unsubscribe_request(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const UnsubscribeResponse& msg);
[[nodiscard]] std::optional<UnsubscribeResponse> decode_unsubscribe_response(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const SubscriptionEvent& msg);
[[nodiscard]] std::optional<SubscriptionEvent> decode_subscription_event(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const UnpublishRequest& msg);
[[nodiscard]] std::optional<UnpublishRequest> decode_unpublish_request(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const UnpublishResponse& msg);
[[nodiscard]] std::optional<UnpublishResponse> decode_unpublish_response(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const HeartbeatRequest& msg);
[[nodiscard]] std::optional<HeartbeatRequest> decode_heartbeat_request(std::span<const uint8_t> data);

[[nodiscard]] std::vector<uint8_t> encode(const ErrorResponse& msg);
[[nodiscard]] std::optional<ErrorResponse> decode_error_response(std::span<const uint8_t> data);

}  // namespace zerokv::core::detail
