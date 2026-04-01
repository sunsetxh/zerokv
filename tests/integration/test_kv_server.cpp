#include "zerokv/kv.h"
#include "kv/protocol.h"
#include "kv/tcp_transport.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using zerokv::kv::KVServer;
using zerokv::kv::ServerConfig;
namespace proto = zerokv::kv::detail;

template <typename Request>
std::vector<uint8_t> make_frame(proto::MsgType type, uint64_t request_id, const Request& req) {
    auto payload = proto::encode(req);
    proto::MsgHeader header;
    header.type = static_cast<uint16_t>(type);
    header.request_id = request_id;
    return proto::encode_message(header, payload);
}

std::pair<proto::MsgHeader, std::vector<uint8_t>> recv_frame(int fd) {
    std::array<uint8_t, proto::kHeaderWireSize> header_buf{};
    EXPECT_TRUE(proto::TcpTransport::recv_exact(fd, header_buf));
    auto header = proto::decode_header(header_buf);
    EXPECT_TRUE(header.has_value());
    std::vector<uint8_t> payload(header->payload_length);
    if (!payload.empty()) {
        EXPECT_TRUE(proto::TcpTransport::recv_exact(fd, std::span<uint8_t>(payload)));
    }
    return {*header, std::move(payload)};
}

TEST(KvServerIntegrationTest, HandlesRegisterPutLookupAndUnpublish) {
    auto cfg = zerokv::Config::builder()
                   .set_transport("tcp")
                   .build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    EXPECT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());
    ASSERT_TRUE(server->is_running());
    ASSERT_FALSE(server->address().empty());

    std::string error;
    int fd = proto::TcpTransport::connect(server->address(), &error);
    ASSERT_GE(fd, 0) << error;

    proto::RegisterNodeRequest reg;
    reg.node_id = "client-a";
    reg.control_addr = "127.0.0.1:19000";
    reg.data_addr = "127.0.0.1:20000";
    reg.push_control_addr = "127.0.0.1:21000";
    reg.subscription_control_addr = "127.0.0.1:22000";
    reg.push_inbox_remote_addr = 0x3000;
    reg.push_inbox_rkey = {9, 9, 9, 9};
    reg.push_inbox_capacity = 8192;
    auto reg_frame = make_frame(proto::MsgType::kRegisterNode, 1, reg);
    ASSERT_TRUE(proto::TcpTransport::send_all(fd, reg_frame));

    auto [reg_header, reg_payload] = recv_frame(fd);
    EXPECT_EQ(static_cast<proto::MsgType>(reg_header.type), proto::MsgType::kRegisterNodeResp);
    auto reg_resp = proto::decode_register_node_response(reg_payload);
    ASSERT_TRUE(reg_resp.has_value());
    EXPECT_EQ(reg_resp->status, proto::MsgStatus::kOk);
    EXPECT_EQ(reg_resp->assigned_node_id, "client-a");

    proto::PutMetaRequest put;
    put.metadata.key = "alpha";
    put.metadata.owner_node_id = "client-a";
    put.metadata.owner_data_addr = "127.0.0.1:20000";
    put.metadata.remote_addr = 0x1234;
    put.metadata.rkey = {1, 2, 3, 4};
    put.metadata.size = 4096;
    put.metadata.version = 5;
    auto put_frame = make_frame(proto::MsgType::kPutMeta, 2, put);
    ASSERT_TRUE(proto::TcpTransport::send_all(fd, put_frame));

    auto [put_header, put_payload] = recv_frame(fd);
    EXPECT_EQ(static_cast<proto::MsgType>(put_header.type), proto::MsgType::kPutMetaResp);
    auto put_resp = proto::decode_put_meta_response(put_payload);
    ASSERT_TRUE(put_resp.has_value());
    EXPECT_EQ(put_resp->status, proto::MsgStatus::kOk);

    auto info = server->lookup("alpha");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->key, "alpha");
    EXPECT_EQ(info->size, 4096u);
    EXPECT_EQ(info->version, 5u);

    proto::GetMetaRequest get;
    get.key = "alpha";
    auto get_frame = make_frame(proto::MsgType::kGetMeta, 3, get);
    ASSERT_TRUE(proto::TcpTransport::send_all(fd, get_frame));

    auto [get_header, get_payload] = recv_frame(fd);
    EXPECT_EQ(static_cast<proto::MsgType>(get_header.type), proto::MsgType::kGetMetaResp);
    auto get_resp = proto::decode_get_meta_response(get_payload);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, proto::MsgStatus::kOk);
    ASSERT_TRUE(get_resp->metadata.has_value());
    EXPECT_EQ(get_resp->metadata->key, "alpha");
    EXPECT_EQ(get_resp->metadata->owner_node_id, "client-a");

    proto::UnpublishRequest unpublish;
    unpublish.key = "alpha";
    unpublish.owner_node_id = "client-a";
    auto del_frame = make_frame(proto::MsgType::kUnpublish, 4, unpublish);
    ASSERT_TRUE(proto::TcpTransport::send_all(fd, del_frame));

    auto [del_header, del_payload] = recv_frame(fd);
    EXPECT_EQ(static_cast<proto::MsgType>(del_header.type), proto::MsgType::kUnpublishResp);
    auto del_resp = proto::decode_unpublish_response(del_payload);
    ASSERT_TRUE(del_resp.has_value());
    EXPECT_EQ(del_resp->status, proto::MsgStatus::kOk);
    EXPECT_FALSE(server->lookup("alpha").has_value());

    proto::TcpTransport::close_fd(&fd);
    server->stop();
}

TEST(KvServerIntegrationTest, ReturnsPushTargetMetadata) {
    auto cfg = zerokv::Config::builder()
                   .set_transport("tcp")
                   .build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    EXPECT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    std::string error;
    int fd = proto::TcpTransport::connect(server->address(), &error);
    ASSERT_GE(fd, 0) << error;

    proto::RegisterNodeRequest reg;
    reg.node_id = "target-a";
    reg.control_addr = "127.0.0.1:19010";
    reg.data_addr = "127.0.0.1:20010";
    reg.push_control_addr = "127.0.0.1:21010";
    reg.subscription_control_addr = "127.0.0.1:22010";
    reg.push_inbox_remote_addr = 0x4400;
    reg.push_inbox_rkey = {7, 7, 7};
    reg.push_inbox_capacity = 2048;
    auto reg_frame = make_frame(proto::MsgType::kRegisterNode, 11, reg);
    ASSERT_TRUE(proto::TcpTransport::send_all(fd, reg_frame));

    auto [reg_header, reg_payload] = recv_frame(fd);
    EXPECT_EQ(static_cast<proto::MsgType>(reg_header.type), proto::MsgType::kRegisterNodeResp);

    proto::GetPushTargetRequest get;
    get.target_node_id = "target-a";
    auto get_frame = make_frame(proto::MsgType::kGetPushTarget, 12, get);
    ASSERT_TRUE(proto::TcpTransport::send_all(fd, get_frame));

    auto [get_header, get_payload] = recv_frame(fd);
    EXPECT_EQ(static_cast<proto::MsgType>(get_header.type), proto::MsgType::kGetPushTargetResp);
    auto get_resp = proto::decode_get_push_target_response(get_payload);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, proto::MsgStatus::kOk);
    EXPECT_EQ(get_resp->target_node_id, "target-a");
    EXPECT_EQ(get_resp->target_data_addr, "127.0.0.1:20010");
    EXPECT_EQ(get_resp->push_control_addr, "127.0.0.1:21010");
    EXPECT_EQ(get_resp->push_inbox_remote_addr, 0x4400u);
    EXPECT_EQ(get_resp->push_inbox_rkey, (std::vector<uint8_t>{7, 7, 7}));
    EXPECT_EQ(get_resp->push_inbox_capacity, 2048u);

    proto::TcpTransport::close_fd(&fd);
    server->stop();
}

TEST(KvServerIntegrationTest, HandlesSubscribeAndUnsubscribe) {
    auto cfg = zerokv::Config::builder()
                   .set_transport("tcp")
                   .build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    EXPECT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    std::string error;
    int fd = proto::TcpTransport::connect(server->address(), &error);
    ASSERT_GE(fd, 0) << error;

    proto::RegisterNodeRequest reg;
    reg.node_id = "subscriber-a";
    reg.control_addr = "127.0.0.1:19020";
    reg.data_addr = "127.0.0.1:20020";
    reg.push_control_addr = "127.0.0.1:21020";
    reg.subscription_control_addr = "127.0.0.1:22020";
    reg.push_inbox_remote_addr = 0x5500;
    reg.push_inbox_rkey = {5, 5, 5};
    reg.push_inbox_capacity = 4096;
    ASSERT_TRUE(proto::TcpTransport::send_all(fd, make_frame(proto::MsgType::kRegisterNode, 21, reg)));
    auto [reg_header, reg_payload] = recv_frame(fd);
    EXPECT_EQ(static_cast<proto::MsgType>(reg_header.type), proto::MsgType::kRegisterNodeResp);

    proto::SubscribeRequest sub;
    sub.subscriber_node_id = "subscriber-a";
    sub.key = "alpha";
    ASSERT_TRUE(proto::TcpTransport::send_all(fd, make_frame(proto::MsgType::kSubscribe, 22, sub)));

    auto [sub_header, sub_payload] = recv_frame(fd);
    EXPECT_EQ(static_cast<proto::MsgType>(sub_header.type), proto::MsgType::kSubscribeResp);
    auto sub_resp = proto::decode_subscribe_response(sub_payload);
    ASSERT_TRUE(sub_resp.has_value());
    EXPECT_EQ(sub_resp->status, proto::MsgStatus::kOk);

    proto::UnsubscribeRequest unsub;
    unsub.subscriber_node_id = "subscriber-a";
    unsub.key = "alpha";
    ASSERT_TRUE(proto::TcpTransport::send_all(fd, make_frame(proto::MsgType::kUnsubscribe, 23, unsub)));

    auto [unsub_header, unsub_payload] = recv_frame(fd);
    EXPECT_EQ(static_cast<proto::MsgType>(unsub_header.type), proto::MsgType::kUnsubscribeResp);
    auto unsub_resp = proto::decode_unsubscribe_response(unsub_payload);
    ASSERT_TRUE(unsub_resp.has_value());
    EXPECT_EQ(unsub_resp->status, proto::MsgStatus::kOk);

    proto::TcpTransport::close_fd(&fd);
    server->stop();
}

TEST(KvServerIntegrationTest, RejectsSubscribeWithEmptyKey) {
    auto cfg = zerokv::Config::builder()
                   .set_transport("tcp")
                   .build();

    auto server = KVServer::create(cfg);
    ASSERT_NE(server, nullptr);
    EXPECT_TRUE(server->start(ServerConfig{"127.0.0.1:0"}).ok());

    std::string error;
    int fd = proto::TcpTransport::connect(server->address(), &error);
    ASSERT_GE(fd, 0) << error;

    proto::RegisterNodeRequest reg;
    reg.node_id = "subscriber-b";
    reg.control_addr = "127.0.0.1:19021";
    reg.data_addr = "127.0.0.1:20021";
    reg.push_control_addr = "127.0.0.1:21021";
    reg.subscription_control_addr = "127.0.0.1:22021";
    reg.push_inbox_remote_addr = 0x5600;
    reg.push_inbox_rkey = {6, 6, 6};
    reg.push_inbox_capacity = 4096;
    ASSERT_TRUE(proto::TcpTransport::send_all(fd, make_frame(proto::MsgType::kRegisterNode, 24, reg)));
    auto [reg_header, reg_payload] = recv_frame(fd);
    EXPECT_EQ(static_cast<proto::MsgType>(reg_header.type), proto::MsgType::kRegisterNodeResp);

    proto::SubscribeRequest sub;
    sub.subscriber_node_id = "subscriber-b";
    sub.key.clear();
    ASSERT_TRUE(proto::TcpTransport::send_all(fd, make_frame(proto::MsgType::kSubscribe, 25, sub)));

    auto [sub_header, sub_payload] = recv_frame(fd);
    EXPECT_EQ(static_cast<proto::MsgType>(sub_header.type), proto::MsgType::kSubscribeResp);
    auto sub_resp = proto::decode_subscribe_response(sub_payload);
    ASSERT_TRUE(sub_resp.has_value());
    EXPECT_EQ(sub_resp->status, proto::MsgStatus::kInvalidRequest);

    proto::TcpTransport::close_fd(&fd);
    server->stop();
}

}  // namespace
