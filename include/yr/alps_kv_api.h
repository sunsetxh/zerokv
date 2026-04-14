#pragma once

#include <cstddef>
#include <vector>

namespace YR {

int SetClient(const char* host, int port, int connect_timeout_ms);
void ShutdownClient();

int WriteBytes(const void* data, size_t size, int tag, int index, int src, int dst);
void ReadBytes(void* data, size_t size, int tag, int index, int src, int dst);
void ReadBytesBatch(std::vector<void*>& data,
                    const std::vector<size_t>& sizes,
                    const std::vector<int>& tags,
                    const std::vector<int>& indices,
                    const std::vector<int>& srcs,
                    const std::vector<int>& dsts);

}  // namespace YR
