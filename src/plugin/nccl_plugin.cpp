/// @file src/plugin/nccl_plugin.cpp
/// @brief Reference NCCL plugin implementation skeleton.
///
/// This file shows how a real NCCL plugin would implement the
/// CollectivePlugin interface.  Build as a shared library:
///
///   g++ -shared -fPIC -o libzerokv_plugin_nccl.so nccl_plugin.cpp -lnccl -lcudart

#include "zerokv/plugin/plugin.h"

#include <cassert>
#include <mutex>
#include <unordered_map>

// Placeholder: in a real build these come from NCCL/CUDA headers.
// #include <nccl.h>
// #include <cuda_runtime.h>

namespace zerokv {
namespace plugin {

// ---------------------------------------------------------------------------
// NcclCommunicator
// ---------------------------------------------------------------------------

class NcclCommunicator : public Communicator {
public:
    NcclCommunicator(void* nccl_comm, int size, int rank)
        : nccl_comm_(nccl_comm), size_(size), rank_(rank) {}

    ~NcclCommunicator() override {
        // ncclCommDestroy(static_cast<ncclComm_t>(nccl_comm_));
    }

    int size() const override { return size_; }
    int rank() const override { return rank_; }
    void* native_handle() const override { return nccl_comm_; }

private:
    void* nccl_comm_;
    int   size_;
    int   rank_;
};

// ---------------------------------------------------------------------------
// NcclPlugin
// ---------------------------------------------------------------------------

class NcclPlugin : public CollectivePlugin {
public:
    NcclPlugin() = default;
    ~NcclPlugin() override { shutdown(); }

    // --- Metadata ------------------------------------------------------------

    const char* name() const noexcept override { return "nccl"; }
    const char* version() const noexcept override { return "2.20.0"; }

    std::vector<MemoryType> supported_memory_types() const override {
        return {MemoryType::kCuda};
    }

    // --- Lifecycle -----------------------------------------------------------

    Status init(const Context::Ptr& ctx) override {
        ctx_ = ctx;
        // ncclGetVersion(&nccl_version_);
        // Verify NCCL version compatibility, check GPU topology, etc.
        return Status::OK();
    }

    Status shutdown() override {
        std::lock_guard lock(mu_);
        comms_.clear();
        ctx_.reset();
        return Status::OK();
    }

    // --- Communicator management ---------------------------------------------

    std::pair<std::vector<uint8_t>, Status> generate_unique_id() override {
        // ncclUniqueId id;
        // ncclGetUniqueId(&id);
        // return {{id.internal, id.internal + sizeof(id)}, Status::OK()};

        // Placeholder:
        std::vector<uint8_t> id(128, 0);
        return {std::move(id), Status::OK()};
    }

    std::pair<Communicator::Ptr, Status>
    create_communicator(const std::vector<uint8_t>& unique_id,
                        int nranks, int rank) override {
        // ncclUniqueId id;
        // std::memcpy(&id, unique_id.data(), sizeof(id));
        // ncclComm_t comm;
        // ncclResult_t res = ncclCommInitRank(&comm, nranks, id, rank);
        // if (res != ncclSuccess) {
        //     return {nullptr, {ErrorCode::kPluginInitFailed, ncclGetErrorString(res)}};
        // }

        void* fake_comm = nullptr;  // placeholder
        auto comm = std::make_shared<NcclCommunicator>(fake_comm, nranks, rank);

        std::lock_guard lock(mu_);
        comms_[comm.get()] = comm;

        return {std::move(comm), Status::OK()};
    }

    Status destroy_communicator(Communicator::Ptr comm) override {
        std::lock_guard lock(mu_);
        comms_.erase(comm.get());
        return Status::OK();
    }

    // --- Collectives ---------------------------------------------------------

    Status allreduce(Communicator::Ptr comm,
                     const void* sendbuf, void* recvbuf,
                     size_t count, DataType dtype,
                     ReduceOp op, void* stream) override {
        // auto nccl_comm = static_cast<ncclComm_t>(comm->native_handle());
        // auto nccl_dtype = to_nccl_dtype(dtype);
        // auto nccl_op    = to_nccl_op(op);
        // auto cuda_stream = static_cast<cudaStream_t>(stream);
        //
        // ncclResult_t res = ncclAllReduce(
        //     sendbuf, recvbuf, count, nccl_dtype, nccl_op, nccl_comm, cuda_stream
        // );
        // return nccl_to_status(res);

        (void)comm; (void)sendbuf; (void)recvbuf;
        (void)count; (void)dtype; (void)op; (void)stream;
        return Status::OK();
    }

    Status broadcast(Communicator::Ptr comm,
                     const void* sendbuf, void* recvbuf,
                     size_t count, DataType dtype,
                     int root, void* stream) override {
        (void)comm; (void)sendbuf; (void)recvbuf;
        (void)count; (void)dtype; (void)root; (void)stream;
        return Status::OK();
    }

    Status allgather(Communicator::Ptr comm,
                     const void* sendbuf, void* recvbuf,
                     size_t sendcount, DataType dtype,
                     void* stream) override {
        (void)comm; (void)sendbuf; (void)recvbuf;
        (void)sendcount; (void)dtype; (void)stream;
        return Status::OK();
    }

    Status reduce_scatter(Communicator::Ptr comm,
                          const void* sendbuf, void* recvbuf,
                          size_t recvcount, DataType dtype,
                          ReduceOp op, void* stream) override {
        (void)comm; (void)sendbuf; (void)recvbuf;
        (void)recvcount; (void)dtype; (void)op; (void)stream;
        return Status::OK();
    }

    Status alltoall(Communicator::Ptr comm,
                    const void* sendbuf, void* recvbuf,
                    size_t count, DataType dtype,
                    void* stream) override {
        (void)comm; (void)sendbuf; (void)recvbuf;
        (void)count; (void)dtype; (void)stream;
        return Status::OK();
    }

    Status send(Communicator::Ptr comm,
                const void* sendbuf, size_t count,
                DataType dtype, int peer, void* stream) override {
        (void)comm; (void)sendbuf; (void)count;
        (void)dtype; (void)peer; (void)stream;
        return Status::OK();
    }

    Status recv(Communicator::Ptr comm,
                void* recvbuf, size_t count,
                DataType dtype, int peer, void* stream) override {
        (void)comm; (void)recvbuf; (void)count;
        (void)dtype; (void)peer; (void)stream;
        return Status::OK();
    }

    Status group_start() override {
        // return nccl_to_status(ncclGroupStart());
        return Status::OK();
    }

    Status group_end() override {
        // return nccl_to_status(ncclGroupEnd());
        return Status::OK();
    }

    // --- Transport integration -----------------------------------------------

    Status register_transport(int peer_rank,
                              std::shared_ptr<Endpoint> ep) override {
        // Store the AXON endpoint for hybrid communication patterns.
        // For example, NCCL can fall back to AXON transport for cross-network
        // communication or when direct GPU-GPU paths are unavailable.
        std::lock_guard lock(mu_);
        transport_eps_[peer_rank] = std::move(ep);
        return Status::OK();
    }

private:
    Context::Ptr ctx_;
    std::mutex   mu_;
    std::unordered_map<Communicator*, Communicator::Ptr> comms_;
    std::unordered_map<int, std::shared_ptr<Endpoint>> transport_eps_;
};

}  // namespace plugin
}  // namespace zerokv

// ---------------------------------------------------------------------------
// C factory function – the single entry point loaded by PluginRegistry
// ---------------------------------------------------------------------------

ZEROKV_PLUGIN_EXPORT
zerokv::plugin::CollectivePlugin* zerokv_plugin_create() {
    return new zerokv::plugin::NcclPlugin();
}
