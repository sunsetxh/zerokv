#include "zerokv/message_kv.h"

namespace zerokv {

struct MessageKV::Impl {};

MessageKV::MessageKV(const zerokv::Config&) : impl_(std::make_unique<Impl>()) {}
MessageKV::~MessageKV() = default;

MessageKV::Ptr MessageKV::create(const zerokv::Config& cfg) {
    return Ptr(new MessageKV(cfg));
}

void MessageKV::start(const zerokv::kv::NodeConfig&) {}

void MessageKV::stop() {}

void MessageKV::send(const std::string&, const void*, size_t) {}

void MessageKV::send_region(const std::string&, const zerokv::MemoryRegion::Ptr&, size_t) {}

void MessageKV::recv(const std::string&,
                     const zerokv::MemoryRegion::Ptr&,
                     size_t,
                     size_t,
                     std::chrono::milliseconds) {}

MessageKV::BatchRecvResult MessageKV::recv_batch(const std::vector<BatchRecvItem>&,
                                                 const zerokv::MemoryRegion::Ptr&,
                                                 std::chrono::milliseconds) {
    return {};
}

}  // namespace zerokv
