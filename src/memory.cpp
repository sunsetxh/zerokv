#include "zerokv/memory.h"

#include "zerokv/config.h"
#include "zerokv/common.h"

#include <ucp/api/ucp.h>

#include <cstdlib>
#include <cstring>
#include <vector>

namespace zerokv {

// ============================================================================
// MemoryRegion::Impl
// ============================================================================

struct MemoryRegion::Impl {
    Context::Ptr context_;
    void* address_ = nullptr;
    size_t length_ = 0;
    MemoryType memory_type_ = MemoryType::kHost;
    ucp_mem_h handle_ = nullptr;
    bool needs_free_ = false;  // Only free if allocated by allocate()

    ~Impl() {
        if (handle_ && context_) {
            ucp_mem_unmap(static_cast<ucp_context_h>(context_->native_handle()), handle_);
        }
        if (needs_free_ && address_) {
            std::free(address_);
        }
    }
};

// ============================================================================
// MemoryRegion
// ============================================================================

MemoryRegion::~MemoryRegion() = default;

MemoryRegion::Ptr MemoryRegion::register_mem(const Context::Ptr& ctx,
                                             void* addr, size_t length,
                                             MemoryType type) {
    if (!ctx || !addr || length == 0) {
        return nullptr;
    }

    ucp_mem_map_params_t params = {};
    params.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS |
                       UCP_MEM_MAP_PARAM_FIELD_LENGTH |
                       UCP_MEM_MAP_PARAM_FIELD_FLAGS;
    params.address = addr;
    params.length = length;
    params.flags = UCP_MEM_MAP_NONBLOCK;

    ucp_mem_h memh = nullptr;
    ucs_status_t status = ucp_mem_map(
        static_cast<ucp_context_h>(ctx->native_handle()),
        &params, &memh);

    if (status != UCS_OK) {
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->context_ = ctx;
    impl->address_ = addr;
    impl->length_ = length;
    impl->memory_type_ = type;
    impl->handle_ = memh;

    auto result = Ptr(new MemoryRegion());
    result->impl_ = std::move(impl);
    return result;
}

MemoryRegion::Ptr MemoryRegion::allocate(const Context::Ptr& ctx,
                                        size_t length,
                                        MemoryType type) {
    if (!ctx || length == 0) {
        return nullptr;
    }

    constexpr size_t kAlignment = 4096;
    const size_t aligned_length = ((length + kAlignment - 1) / kAlignment) * kAlignment;

    void* addr = std::aligned_alloc(kAlignment, aligned_length);
    if (!addr) {
        return nullptr;
    }

    std::memset(addr, 0, length);

    // Register memory
    ucp_mem_map_params_t params = {};
    params.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS |
                       UCP_MEM_MAP_PARAM_FIELD_LENGTH |
                       UCP_MEM_MAP_PARAM_FIELD_FLAGS;
    params.address = addr;
    params.length = length;
    params.flags = UCP_MEM_MAP_NONBLOCK;

    ucp_mem_h memh = nullptr;
    ucs_status_t status = ucp_mem_map(
        static_cast<ucp_context_h>(ctx->native_handle()),
        &params, &memh);

    if (status != UCS_OK) {
        std::free(addr);
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->context_ = ctx;
    impl->address_ = addr;
    impl->length_ = length;
    impl->memory_type_ = type;
    impl->handle_ = memh;
    impl->needs_free_ = true;  // Mark as needing free

    auto result = Ptr(new MemoryRegion());
    result->impl_ = std::move(impl);
    return result;
}

void* MemoryRegion::address() const noexcept {
    return impl_ ? impl_->address_ : nullptr;
}

size_t MemoryRegion::length() const noexcept {
    return impl_ ? impl_->length_ : 0;
}

MemoryType MemoryRegion::memory_type() const noexcept {
    return impl_ ? impl_->memory_type_ : MemoryType::kHost;
}

RemoteKey MemoryRegion::remote_key() const {
    if (!impl_ || !impl_->handle_ || !impl_->context_) {
        return RemoteKey{};
    }

    void* rkey_buffer = nullptr;
    size_t rkey_size = 0;
    ucs_status_t status = ucp_rkey_pack(
        static_cast<ucp_context_h>(impl_->context_->native_handle()),
        impl_->handle_,
        &rkey_buffer,
        &rkey_size);

    if (status != UCS_OK) {
        return RemoteKey{};
    }

    RemoteKey rkey;
    rkey.data.resize(rkey_size);
    std::memcpy(rkey.data.data(), rkey_buffer, rkey_size);
    ucp_rkey_buffer_release(rkey_buffer);
    return rkey;
}

void* MemoryRegion::native_handle() const noexcept {
    return impl_ ? reinterpret_cast<void*>(impl_->handle_) : nullptr;
}

} // namespace zerokv
