#ifndef ZEROKV_COMMON_H
#define ZEROKV_COMMON_H

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace zerokv {

// Memory types for different transport selection
enum class MemoryType {
    CPU,        // CPU memory → UCX
    HUAWEI_NPU, // Huawei NPU → HCCL
    NVIDIA_GPU  // NVIDIA GPU → NCCL
};

// Operation codes
enum class OpCode : uint8_t {
    PUT = 0,
    GET = 1,
    DELETE = 2,
    PUT_USER_MEM = 3,
    GET_USER_MEM = 4
};

// Status codes
enum class Status : uint8_t {
    OK = 0,
    NOT_FOUND = 1,
    ERROR = 2,
    TIMEOUT = 3,
    OUT_OF_MEMORY = 4,
    INVALID_MEMORY = 5
};

// Result wrapper
template<typename T>
class Result {
public:
    Result(Status s, T&& v) : status(s), value(std::move(v)) {}
    Result(Status s) : status(s) {}

    bool ok() const { return status == Status::OK; }
    Status status;
    T value;
};

using Buffer = std::vector<uint8_t>;

} // namespace zerokv

#endif // ZEROKV_COMMON_H
