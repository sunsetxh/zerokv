#include "zerokv/message_kv.h"

#include <mutex>

namespace zerokv {

struct MessageKV::Impl {
    explicit Impl(const zerokv::Config& config) : cfg(config) {}

    zerokv::Config cfg;
    zerokv::kv::KVNode::Ptr node;
    std::mutex mu;
    bool running = false;
};

MessageKV::MessageKV(const zerokv::Config& cfg) : impl_(std::make_unique<Impl>(cfg)) {}
MessageKV::~MessageKV() = default;

MessageKV::Ptr MessageKV::create(const zerokv::Config& cfg) {
    return Ptr(new MessageKV(cfg));
}

void MessageKV::start(const zerokv::kv::NodeConfig& cfg) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->running) {
        return;
    }

    impl_->node = zerokv::kv::KVNode::create(impl_->cfg);
    auto status = impl_->node->start(cfg);
    status.throw_if_error();
    impl_->running = true;
}

void MessageKV::stop() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->running) {
        return;
    }

    impl_->node->stop();
    impl_->node.reset();
    impl_->running = false;
}

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
