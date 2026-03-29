#include "kv/tcp_framing.h"

#include "kv/tcp_transport.h"

#include <array>

namespace axon::kv::detail {

bool send_frame(int fd, const MsgHeader& header, std::span<const uint8_t> payload) {
    return TcpTransport::send_all(fd, encode_message(header, payload));
}

bool send_frame(int fd, MsgType type, uint64_t request_id, std::span<const uint8_t> payload) {
    MsgHeader header;
    header.type = static_cast<uint16_t>(type);
    header.request_id = request_id;
    return send_frame(fd, header, payload);
}

bool recv_frame(int fd, MsgHeader* header, std::vector<uint8_t>* payload) {
    if (header == nullptr || payload == nullptr) {
        return false;
    }

    std::array<uint8_t, kHeaderWireSize> header_buf{};
    if (!TcpTransport::recv_exact(fd, header_buf)) {
        return false;
    }

    auto decoded_header = decode_header(header_buf);
    if (!decoded_header.has_value()) {
        return false;
    }

    payload->assign(decoded_header->payload_length, 0);
    if (!payload->empty() && !TcpTransport::recv_exact(fd, std::span<uint8_t>(*payload))) {
        return false;
    }

    *header = *decoded_header;
    return true;
}

}  // namespace axon::kv::detail
