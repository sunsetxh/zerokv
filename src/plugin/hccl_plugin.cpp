/// @file src/plugin/hccl_plugin.cpp
/// @brief HCCL (Huawei Collective Communication Library) plugin for Ascend NPU.
///
/// This plugin bridges the AXON transport layer with HCCL for collective and
/// point-to-point communication on Huawei Ascend NPU devices.
///
/// Dependencies:
///   - CANN SDK (ACL runtime): libascendcl.so
///   - HCCL library: libhccl.so
///
/// Build as a shared library:
///   g++ -shared -fPIC -o libaxon_plugin_hccl.so hccl_plugin.cpp \
///       -lhccl -lascendcl -I${ASCEND_HOME}/include

#include "axon/plugin/plugin.h"

#include <cassert>
#include <cstring>
#include <mutex>
#include <unordered_map>

// ---------------------------------------------------------------------------
// HCCL / ACL headers (from CANN SDK)
// In a real build, uncomment these and remove the placeholder declarations.
// ---------------------------------------------------------------------------
// #include <acl/acl.h>            // aclInit, aclFinalize, aclrtSetDevice, ...
// #include <hccl/hccl.h>          // HcclAllReduce, HcclSend, HcclRecv, ...
// #include <hccl/hccl_types.h>    // HcclDataType, HcclReduceOp, HcclResult, ...

// ---------------------------------------------------------------------------
// Placeholder type/enum declarations (mirrors CANN SDK hccl_types.h + acl.h)
// Remove this block when building against real CANN headers.
// ---------------------------------------------------------------------------
#if !defined(HCCL_H)

using aclrtStream = void*;
using HcclComm    = void*;

struct HcclRootInfo {
    char internal[4108];
};

enum HcclResult {
    HCCL_SUCCESS              = 0,
    HCCL_E_PARA               = 1,
    HCCL_E_PTR                = 2,
    HCCL_E_MEMORY             = 3,
    HCCL_E_INTERNAL           = 4,
    HCCL_E_NOT_SUPPORT        = 5,
    HCCL_E_NOT_FOUND          = 6,
    HCCL_E_UNAVAIL            = 7,
    HCCL_E_SYSCALL            = 8,
    HCCL_E_TIMEOUT            = 9,
    HCCL_E_OPEN_FILE_FAILURE  = 10,
    HCCL_E_TCP_CONNECT        = 11,
    HCCL_E_ROCE_CONNECT       = 12,
    HCCL_E_TCP_TRANSFER       = 13,
    HCCL_E_ROCE_TRANSFER      = 14,
    HCCL_E_RUNTIME            = 15,
    HCCL_E_DRV                = 16,
    HCCL_E_PROFILING           = 17,
    HCCL_E_CCE                = 18,
    HCCL_E_NETWORK            = 19,
    HCCL_E_AGAIN              = 20,
    HCCL_E_RESERVED           = 21,
};

enum HcclDataType {
    HCCL_DATA_TYPE_INT8    = 0,
    HCCL_DATA_TYPE_INT16   = 1,
    HCCL_DATA_TYPE_INT32   = 2,
    HCCL_DATA_TYPE_FP16    = 3,
    HCCL_DATA_TYPE_FP32    = 4,
    HCCL_DATA_TYPE_INT64   = 5,
    HCCL_DATA_TYPE_UINT64  = 6,
    HCCL_DATA_TYPE_UINT8   = 7,
    HCCL_DATA_TYPE_UINT16  = 8,
    HCCL_DATA_TYPE_UINT32  = 9,
    HCCL_DATA_TYPE_FP64    = 10,
    HCCL_DATA_TYPE_BFP16   = 11,
};

enum HcclReduceOp {
    HCCL_REDUCE_SUM  = 0,
    HCCL_REDUCE_PROD = 1,
    HCCL_REDUCE_MAX  = 2,
    HCCL_REDUCE_MIN  = 3,
};

// Placeholder HCCL function stubs (replaced by -lhccl at link time).
inline HcclResult HcclGetRootInfo(HcclRootInfo*) { return HCCL_SUCCESS; }
inline HcclResult HcclCommInitRootInfo(uint32_t, HcclRootInfo*, uint32_t, HcclComm*) { return HCCL_SUCCESS; }
inline HcclResult HcclCommDestroy(HcclComm) { return HCCL_SUCCESS; }
inline HcclResult HcclAllReduce(void*, void*, uint64_t, HcclDataType, HcclReduceOp, HcclComm, aclrtStream) { return HCCL_SUCCESS; }
inline HcclResult HcclBroadcast(void*, uint64_t, HcclDataType, uint32_t, HcclComm, aclrtStream) { return HCCL_SUCCESS; }
inline HcclResult HcclAllGather(void*, void*, uint64_t, HcclDataType, HcclComm, aclrtStream) { return HCCL_SUCCESS; }
inline HcclResult HcclReduceScatter(void*, void*, uint64_t, HcclDataType, HcclReduceOp, HcclComm, aclrtStream) { return HCCL_SUCCESS; }
inline HcclResult HcclAlltoAll(void*, void*, uint64_t, HcclDataType, HcclComm, aclrtStream) { return HCCL_SUCCESS; }
inline HcclResult HcclSend(void*, uint64_t, HcclDataType, uint32_t, HcclComm, aclrtStream) { return HCCL_SUCCESS; }
inline HcclResult HcclRecv(void*, uint64_t, HcclDataType, uint32_t, HcclComm, aclrtStream) { return HCCL_SUCCESS; }

// Placeholder ACL function stubs.
inline int aclInit(const char*) { return 0; }
inline int aclFinalize() { return 0; }
inline int aclrtSetDevice(int32_t) { return 0; }
inline int aclrtResetDevice(int32_t) { return 0; }
inline int aclrtCreateStream(aclrtStream*) { return 0; }
inline int aclrtDestroyStream(aclrtStream) { return 0; }
inline int aclrtSynchronizeStream(aclrtStream) { return 0; }

#endif  // !HCCL_H

namespace axon {
namespace plugin {

// ---------------------------------------------------------------------------
// Helper: HCCL result -> AXON Status
// ---------------------------------------------------------------------------

static Status hccl_to_status(HcclResult res) {
    if (res == HCCL_SUCCESS) {
        return Status::OK();
    }
    // Map HCCL error codes to AXON error codes.
    ErrorCode ec;
    switch (res) {
        case HCCL_E_PARA:
        case HCCL_E_PTR:
            ec = ErrorCode::kInvalidArgument;
            break;
        case HCCL_E_MEMORY:
            ec = ErrorCode::kOutOfMemory;
            break;
        case HCCL_E_NOT_SUPPORT:
        case HCCL_E_NOT_FOUND:
        case HCCL_E_UNAVAIL:
            ec = ErrorCode::kPluginNotFound;
            break;
        case HCCL_E_TIMEOUT:
            ec = ErrorCode::kTimeout;
            break;
        case HCCL_E_TCP_CONNECT:
        case HCCL_E_ROCE_CONNECT:
            ec = ErrorCode::kConnectionRefused;
            break;
        case HCCL_E_TCP_TRANSFER:
        case HCCL_E_ROCE_TRANSFER:
            ec = ErrorCode::kTransportError;
            break;
        default:
            ec = ErrorCode::kInternalError;
            break;
    }
    return {ec, "HCCL error " + std::to_string(static_cast<int>(res))};
}

// ---------------------------------------------------------------------------
// Helper: AXON DataType -> HcclDataType
// ---------------------------------------------------------------------------

static HcclDataType to_hccl_dtype(DataType dt) {
    switch (dt) {
        case DataType::kFloat16:  return HCCL_DATA_TYPE_FP16;
        case DataType::kFloat32:  return HCCL_DATA_TYPE_FP32;
        case DataType::kFloat64:  return HCCL_DATA_TYPE_FP64;
        case DataType::kBFloat16: return HCCL_DATA_TYPE_BFP16;
        case DataType::kInt8:     return HCCL_DATA_TYPE_INT8;
        case DataType::kInt32:    return HCCL_DATA_TYPE_INT32;
        case DataType::kInt64:    return HCCL_DATA_TYPE_INT64;
        case DataType::kUint8:    return HCCL_DATA_TYPE_UINT8;
    }
    // Should not reach here; default to FP32.
    return HCCL_DATA_TYPE_FP32;
}

// ---------------------------------------------------------------------------
// Helper: AXON ReduceOp -> HcclReduceOp
// ---------------------------------------------------------------------------

static HcclReduceOp to_hccl_op(ReduceOp op) {
    switch (op) {
        case ReduceOp::kSum:  return HCCL_REDUCE_SUM;
        case ReduceOp::kProd: return HCCL_REDUCE_PROD;
        case ReduceOp::kMax:  return HCCL_REDUCE_MAX;
        case ReduceOp::kMin:  return HCCL_REDUCE_MIN;
        case ReduceOp::kAvg:
            // HCCL does not have a native Avg op.
            // Fallback: use SUM and divide by nranks in the caller.
            // TODO(hccl): implement Avg as SUM + scale, or return error.
            return HCCL_REDUCE_SUM;
    }
    return HCCL_REDUCE_SUM;
}

// ---------------------------------------------------------------------------
// HcclCommunicator
// ---------------------------------------------------------------------------

class HcclCommunicator : public Communicator {
public:
    HcclCommunicator(HcclComm hccl_comm, int size, int rank, int device_id)
        : hccl_comm_(hccl_comm), size_(size), rank_(rank),
          device_id_(device_id) {}

    ~HcclCommunicator() override {
        if (hccl_comm_) {
            // Ensure device context is active before destroying.
            aclrtSetDevice(device_id_);
            HcclCommDestroy(hccl_comm_);
            hccl_comm_ = nullptr;
        }
    }

    int size() const override { return size_; }
    int rank() const override { return rank_; }
    void* native_handle() const override { return hccl_comm_; }
    int device_id() const { return device_id_; }

private:
    HcclComm hccl_comm_;
    int      size_;
    int      rank_;
    int      device_id_;
};

// ---------------------------------------------------------------------------
// HcclPlugin
// ---------------------------------------------------------------------------

class HcclPlugin : public CollectivePlugin {
public:
    HcclPlugin() = default;

    ~HcclPlugin() override {
        shutdown();
    }

    // --- Metadata ------------------------------------------------------------

    const char* name() const noexcept override { return "hccl"; }
    const char* version() const noexcept override { return "1.0.0"; }

    std::vector<MemoryType> supported_memory_types() const override {
        return {MemoryType::kAscend};
    }

    // --- Lifecycle -----------------------------------------------------------

    Status init(const Context::Ptr& ctx) override {
        ctx_ = ctx;

        // Initialize ACL runtime.
        // aclInit(nullptr) must be called once per process before any ACL call.
        // In multi-plugin scenarios the caller may have already initialised ACL;
        // aclInit is idempotent in CANN >= 6.0, but guard against double-init
        // in older versions.
        int acl_ret = aclInit(nullptr);
        if (acl_ret != 0) {
            return {ErrorCode::kPluginInitFailed,
                    "aclInit failed with code " + std::to_string(acl_ret)};
        }
        acl_inited_ = true;

        // TODO: Query available Ascend devices via aclrtGetDeviceCount()
        //       and store topology info.
        return Status::OK();
    }

    Status shutdown() override {
        std::lock_guard lock(mu_);

        // Destroy all communicators first.
        comms_.clear();
        transport_eps_.clear();

        // Finalize ACL runtime (only if we initialised it).
        if (acl_inited_) {
            aclFinalize();
            acl_inited_ = false;
        }

        ctx_.reset();
        return Status::OK();
    }

    // --- Communicator management ---------------------------------------------

    std::pair<std::vector<uint8_t>, Status> generate_unique_id() override {
        HcclRootInfo root_info;
        std::memset(&root_info, 0, sizeof(root_info));

        HcclResult res = HcclGetRootInfo(&root_info);
        if (res != HCCL_SUCCESS) {
            return {{}, hccl_to_status(res)};
        }

        // Serialize HcclRootInfo into a byte vector for transport.
        std::vector<uint8_t> id(sizeof(root_info));
        std::memcpy(id.data(), &root_info, sizeof(root_info));
        return {std::move(id), Status::OK()};
    }

    std::pair<Communicator::Ptr, Status>
    create_communicator(const std::vector<uint8_t>& unique_id,
                        int nranks, int rank) override {
        if (unique_id.size() < sizeof(HcclRootInfo)) {
            return {nullptr, {ErrorCode::kInvalidArgument,
                              "unique_id too small for HcclRootInfo"}};
        }

        // Deserialize HcclRootInfo.
        HcclRootInfo root_info;
        std::memcpy(&root_info, unique_id.data(), sizeof(root_info));

        // Set the Ascend device for this rank.
        // Convention: device_id = rank % device_count, or caller-configurable.
        // TODO: make device_id configurable via Context/Config.
        int device_id = rank;  // simplistic; real code should query topology.
        int acl_ret = aclrtSetDevice(device_id);
        if (acl_ret != 0) {
            return {nullptr, {ErrorCode::kPluginInitFailed,
                              "aclrtSetDevice(" + std::to_string(device_id) +
                              ") failed: " + std::to_string(acl_ret)}};
        }

        // Initialize HCCL communicator.
        HcclComm hccl_comm = nullptr;
        HcclResult res = HcclCommInitRootInfo(
            static_cast<uint32_t>(nranks),
            &root_info,
            static_cast<uint32_t>(rank),
            &hccl_comm);
        if (res != HCCL_SUCCESS) {
            return {nullptr, hccl_to_status(res)};
        }

        auto comm = std::make_shared<HcclCommunicator>(
            hccl_comm, nranks, rank, device_id);

        std::lock_guard lock(mu_);
        comms_[comm.get()] = comm;

        return {std::move(comm), Status::OK()};
    }

    Status destroy_communicator(Communicator::Ptr comm) override {
        std::lock_guard lock(mu_);
        comms_.erase(comm.get());
        // The shared_ptr destructor of HcclCommunicator will call
        // HcclCommDestroy when the last reference is released.
        return Status::OK();
    }

    // --- Collective operations -----------------------------------------------
    //
    // Note on stream parameter:
    //   The void* stream in the plugin interface is cast to aclrtStream.
    //   The caller must create the stream via aclrtCreateStream() and pass
    //   it as void*.  This mirrors how NCCL uses cudaStream_t.
    //

    Status allreduce(Communicator::Ptr comm,
                     const void* sendbuf, void* recvbuf,
                     size_t count, DataType dtype,
                     ReduceOp op, void* stream) override {
        auto* hcomm = static_cast<HcclCommunicator*>(comm.get());
        aclrtSetDevice(hcomm->device_id());

        HcclResult res = HcclAllReduce(
            const_cast<void*>(sendbuf),  // HCCL API takes non-const void*
            recvbuf,
            static_cast<uint64_t>(count),
            to_hccl_dtype(dtype),
            to_hccl_op(op),
            static_cast<HcclComm>(hcomm->native_handle()),
            static_cast<aclrtStream>(stream));
        return hccl_to_status(res);
    }

    Status broadcast(Communicator::Ptr comm,
                     const void* sendbuf, void* recvbuf,
                     size_t count, DataType dtype,
                     int root, void* stream) override {
        auto* hcomm = static_cast<HcclCommunicator*>(comm.get());
        aclrtSetDevice(hcomm->device_id());

        // HCCL Broadcast signature:
        //   HcclBroadcast(void* buf, uint64_t count, HcclDataType,
        //                 uint32_t root, HcclComm, aclrtStream)
        //
        // NOTE: HCCL Broadcast is in-place on all ranks.  The root rank's
        //       sendbuf is broadcast to all ranks' buf.  If sendbuf != recvbuf,
        //       we need to copy sendbuf -> recvbuf on the root first.
        //
        // GAP: Our interface separates sendbuf/recvbuf, but HCCL uses a single
        //      buffer. For root rank, we use sendbuf (cast away const); for
        //      non-root ranks, we use recvbuf.
        void* buf = (hcomm->rank() == root)
                        ? const_cast<void*>(sendbuf)
                        : recvbuf;

        HcclResult res = HcclBroadcast(
            buf,
            static_cast<uint64_t>(count),
            to_hccl_dtype(dtype),
            static_cast<uint32_t>(root),
            static_cast<HcclComm>(hcomm->native_handle()),
            static_cast<aclrtStream>(stream));
        return hccl_to_status(res);
    }

    Status allgather(Communicator::Ptr comm,
                     const void* sendbuf, void* recvbuf,
                     size_t sendcount, DataType dtype,
                     void* stream) override {
        auto* hcomm = static_cast<HcclCommunicator*>(comm.get());
        aclrtSetDevice(hcomm->device_id());

        HcclResult res = HcclAllGather(
            const_cast<void*>(sendbuf),
            recvbuf,
            static_cast<uint64_t>(sendcount),
            to_hccl_dtype(dtype),
            static_cast<HcclComm>(hcomm->native_handle()),
            static_cast<aclrtStream>(stream));
        return hccl_to_status(res);
    }

    Status reduce_scatter(Communicator::Ptr comm,
                          const void* sendbuf, void* recvbuf,
                          size_t recvcount, DataType dtype,
                          ReduceOp op, void* stream) override {
        auto* hcomm = static_cast<HcclCommunicator*>(comm.get());
        aclrtSetDevice(hcomm->device_id());

        HcclResult res = HcclReduceScatter(
            const_cast<void*>(sendbuf),
            recvbuf,
            static_cast<uint64_t>(recvcount),
            to_hccl_dtype(dtype),
            to_hccl_op(op),
            static_cast<HcclComm>(hcomm->native_handle()),
            static_cast<aclrtStream>(stream));
        return hccl_to_status(res);
    }

    Status alltoall(Communicator::Ptr comm,
                    const void* sendbuf, void* recvbuf,
                    size_t count, DataType dtype,
                    void* stream) override {
        auto* hcomm = static_cast<HcclCommunicator*>(comm.get());
        aclrtSetDevice(hcomm->device_id());

        // HcclAlltoAll is available in CANN >= 7.0.
        // Older versions may not support it; check at runtime.
        HcclResult res = HcclAlltoAll(
            const_cast<void*>(sendbuf),
            recvbuf,
            static_cast<uint64_t>(count),
            to_hccl_dtype(dtype),
            static_cast<HcclComm>(hcomm->native_handle()),
            static_cast<aclrtStream>(stream));
        return hccl_to_status(res);
    }

    // --- Point-to-point operations -------------------------------------------
    //
    // HCCL supports HcclSend / HcclRecv since CANN 6.x.
    // These map directly to our plugin interface.

    Status send(Communicator::Ptr comm,
                const void* sendbuf, size_t count,
                DataType dtype, int peer, void* stream) override {
        auto* hcomm = static_cast<HcclCommunicator*>(comm.get());
        aclrtSetDevice(hcomm->device_id());

        HcclResult res = HcclSend(
            const_cast<void*>(sendbuf),
            static_cast<uint64_t>(count),
            to_hccl_dtype(dtype),
            static_cast<uint32_t>(peer),
            static_cast<HcclComm>(hcomm->native_handle()),
            static_cast<aclrtStream>(stream));
        return hccl_to_status(res);
    }

    Status recv(Communicator::Ptr comm,
                void* recvbuf, size_t count,
                DataType dtype, int peer, void* stream) override {
        auto* hcomm = static_cast<HcclCommunicator*>(comm.get());
        aclrtSetDevice(hcomm->device_id());

        HcclResult res = HcclRecv(
            recvbuf,
            static_cast<uint64_t>(count),
            to_hccl_dtype(dtype),
            static_cast<uint32_t>(peer),
            static_cast<HcclComm>(hcomm->native_handle()),
            static_cast<aclrtStream>(stream));
        return hccl_to_status(res);
    }

    // --- Synchronisation -----------------------------------------------------
    //
    // GAP: HCCL does NOT provide HcclGroupStart / HcclGroupEnd.
    //      Unlike NCCL which batches multiple collectives via ncclGroupStart/End,
    //      HCCL executes operations individually on streams.
    //      We return success (no-op) to maintain interface compatibility.
    //      For callers that rely on grouping for performance, this is a
    //      semantic gap that should be documented.

    Status group_start() override {
        // No-op: HCCL has no group batching API.
        return Status::OK();
    }

    Status group_end() override {
        // No-op: HCCL has no group batching API.
        return Status::OK();
    }

    // --- Transport integration -----------------------------------------------

    Status register_transport(int peer_rank,
                              std::shared_ptr<Endpoint> ep) override {
        std::lock_guard lock(mu_);
        transport_eps_[peer_rank] = std::move(ep);
        return Status::OK();
    }

private:
    Context::Ptr ctx_;
    std::mutex   mu_;
    bool         acl_inited_ = false;
    std::unordered_map<Communicator*, Communicator::Ptr> comms_;
    std::unordered_map<int, std::shared_ptr<Endpoint>> transport_eps_;
};

}  // namespace plugin
}  // namespace axon

// ---------------------------------------------------------------------------
// C factory function -- the single entry point loaded by PluginRegistry
// ---------------------------------------------------------------------------

AXON_PLUGIN_EXPORT
axon::plugin::CollectivePlugin* axon_plugin_create() {
    return new axon::plugin::HcclPlugin();
}
