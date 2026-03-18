#pragma once

#include <ucp/api/ucp.h>

#include <memory>
#include <cstddef>
#include <cstdint>

namespace axon {
namespace internal {

/// RAII wrapper for UCX memory registration
class UcxMemory {
public:
    UcxMemory(ucp_context_h context, void* addr, size_t length, uint64_t flags);
    ~UcxMemory();

    // Non-copyable
    UcxMemory(const UcxMemory&) = delete;
    UcxMemory& operator=(const UcxMemory&) = delete;

    // Movable
    UcxMemory(UcxMemory&& other) noexcept;
    UcxMemory& operator=(UcxMemory&& other) noexcept;

    /// Get underlying UCX memory handle
    [[nodiscard]] ucp_mem_h handle() const noexcept { return handle_; }

    /// Check if memory region is valid
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

    /// Get registered address
    [[nodiscard]] void* address() const noexcept { return address_; }

    /// Get registered length
    [[nodiscard]] size_t length() const noexcept { return length_; }

    /// Pack remote key for transfer to peer
    std::vector<uint8_t> pack_rkey() const;

    /// Unpack remote key from peer
    static ucp_rkey_h unpack_rkey(ucp_context_h context,
                                   const uint8_t* buffer, size_t length);

    /// Destroy remote key
    static void destroy_rkey(ucp_rkey_h rkey);

private:
    ucp_context_h context_;
    ucp_mem_h handle_;
    void* address_;
    size_t length_;
};

} // namespace internal
} // namespace axon
