#pragma once

#include "kv/protocol.h"

#include <cstdint>
#include <span>
#include <vector>

namespace axon::kv::detail {

bool send_frame(int fd, const MsgHeader& header, std::span<const uint8_t> payload);
bool send_frame(int fd, MsgType type, uint64_t request_id, std::span<const uint8_t> payload);
bool recv_frame(int fd, MsgHeader* header, std::vector<uint8_t>* payload);

}  // namespace axon::kv::detail
