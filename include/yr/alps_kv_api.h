#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace YR {

struct WriteTimingStats {
    uint64_t write_ops = 0;
    uint64_t control_request_grant_us = 0;
    uint64_t rdma_put_us = 0;
    uint64_t flush_us = 0;
    uint64_t write_done_us = 0;
};

struct ReceivePathStats {
    uint64_t direct_grant_ops = 0;
    uint64_t staged_grant_ops = 0;
    uint64_t staged_delivery_ops = 0;
    uint64_t staged_copy_bytes = 0;
    uint64_t staged_copy_us = 0;
};

int SetClient(const char* host, int port, int connect_timeout_ms);
void ShutdownClient();
std::string GetLocalAddress();
WriteTimingStats GetWriteTimingStats();
void ResetWriteTimingStats();
ReceivePathStats GetReceivePathStats();
void ResetReceivePathStats();
bool WaitForReceiveSlots(size_t expected, int timeout_ms);

int WriteBytes(const void* data, size_t size, int tag, int index, int src, int dst);
void ReadBytes(void* data, size_t size, int tag, int index, int src, int dst);
void ReadBytesBatch(std::vector<void*>& data,
                    const std::vector<size_t>& sizes,
                    const std::vector<int>& tags,
                    const std::vector<int>& indices,
                    const std::vector<int>& srcs,
                    const std::vector<int>& dsts);

}  // namespace YR
