#pragma once

/// @file zerokv/plugin/plugin.h
/// @brief Plugin interface for NCCL/HCCL collective communication backends.
///
/// A Plugin bridges collective communication libraries (NCCL, HCCL) with
/// the AXON transport layer.  The plugin system uses a registration-based
/// discovery model:
///
///   1. Shared libraries implement the CollectivePlugin interface.
///   2. Each .so exports a C factory function: zerokv_plugin_create().
///   3. PluginRegistry discovers and loads plugins at runtime.
///
/// Plugin lifecycle:
///   load .so  ->  zerokv_plugin_create()  ->  init(ctx)  ->  create_comm()
///   ->  allreduce / broadcast / ...  ->  destroy_comm()  ->  shutdown()

#include "zerokv/common.h"
#include "zerokv/memory.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace zerokv {
namespace plugin {

// ---------------------------------------------------------------------------
// Communicator handle
// ---------------------------------------------------------------------------

/// Opaque communicator handle wrapping NCCL/HCCL communicator.
class Communicator {
public:
    using Ptr = std::shared_ptr<Communicator>;

    virtual ~Communicator() = default;

    /// Number of ranks in this communicator.
    [[nodiscard]] virtual int size() const = 0;

    /// Rank of this process.
    [[nodiscard]] virtual int rank() const = 0;

    /// Underlying native communicator (ncclComm_t / HcclComm).
    [[nodiscard]] virtual void* native_handle() const = 0;
};

// ---------------------------------------------------------------------------
// Reduction operations
// ---------------------------------------------------------------------------

enum class ReduceOp : int {
    kSum  = 0,
    kProd = 1,
    kMax  = 2,
    kMin  = 3,
    kAvg  = 4,
};

// ---------------------------------------------------------------------------
// Data type descriptors
// ---------------------------------------------------------------------------

enum class DataType : int {
    kFloat16  = 0,
    kFloat32  = 1,
    kFloat64  = 2,
    kBFloat16 = 3,
    kInt8     = 4,
    kInt32    = 5,
    kInt64    = 6,
    kUint8    = 7,
};

/// Size in bytes for a DataType.
[[nodiscard]] size_t dtype_size(DataType dt) noexcept;

// ---------------------------------------------------------------------------
// CollectivePlugin – the interface plugins must implement
// ---------------------------------------------------------------------------

class CollectivePlugin {
public:
    virtual ~CollectivePlugin() = default;

    // --- Metadata ------------------------------------------------------------

    /// Plugin name (e.g., "nccl", "hccl").
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// Plugin version string.
    [[nodiscard]] virtual const char* version() const noexcept = 0;

    /// Which memory types this plugin supports (e.g., {kCuda} for NCCL).
    [[nodiscard]] virtual std::vector<MemoryType> supported_memory_types() const = 0;

    // --- Lifecycle -----------------------------------------------------------

    /// Initialise the plugin with the AXON context.
    /// Called once, before any other method.
    virtual Status init(const Context::Ptr& ctx) = 0;

    /// Shut down the plugin (release all resources).
    virtual Status shutdown() = 0;

    // --- Communicator management ---------------------------------------------

    /// Create a communicator for a set of ranks.
    /// @param unique_id   Opaque bootstrap ID (e.g. ncclUniqueId bytes).
    /// @param nranks      Total number of ranks.
    /// @param rank        This process's rank.
    virtual std::pair<Communicator::Ptr, Status>
    create_communicator(const std::vector<uint8_t>& unique_id,
                        int nranks, int rank) = 0;

    /// Generate a unique ID for bootstrapping (rank 0 calls this, broadcasts).
    virtual std::pair<std::vector<uint8_t>, Status>
    generate_unique_id() = 0;

    /// Destroy a communicator.
    virtual Status destroy_communicator(Communicator::Ptr comm) = 0;

    // --- Collective operations -----------------------------------------------
    // All collectives are asynchronous; they enqueue work on a device stream
    // and return immediately.  The @p stream parameter is the native stream
    // (cudaStream_t / aclrtStream).

    virtual Status allreduce(Communicator::Ptr comm,
                             const void* sendbuf,
                             void*       recvbuf,
                             size_t      count,
                             DataType    dtype,
                             ReduceOp    op,
                             void*       stream) = 0;

    virtual Status broadcast(Communicator::Ptr comm,
                             const void* sendbuf,
                             void*       recvbuf,
                             size_t      count,
                             DataType    dtype,
                             int         root,
                             void*       stream) = 0;

    virtual Status allgather(Communicator::Ptr comm,
                             const void* sendbuf,
                             void*       recvbuf,
                             size_t      sendcount,
                             DataType    dtype,
                             void*       stream) = 0;

    virtual Status reduce_scatter(Communicator::Ptr comm,
                                  const void* sendbuf,
                                  void*       recvbuf,
                                  size_t      recvcount,
                                  DataType    dtype,
                                  ReduceOp    op,
                                  void*       stream) = 0;

    virtual Status alltoall(Communicator::Ptr comm,
                            const void* sendbuf,
                            void*       recvbuf,
                            size_t      count,
                            DataType    dtype,
                            void*       stream) = 0;

    /// Point-to-point send within a communicator.
    virtual Status send(Communicator::Ptr comm,
                        const void* sendbuf,
                        size_t      count,
                        DataType    dtype,
                        int         peer,
                        void*       stream) = 0;

    /// Point-to-point recv within a communicator.
    virtual Status recv(Communicator::Ptr comm,
                        void*       recvbuf,
                        size_t      count,
                        DataType    dtype,
                        int         peer,
                        void*       stream) = 0;

    // --- Synchronisation -----------------------------------------------------

    /// Group start (batch multiple collectives).
    virtual Status group_start() = 0;

    /// Group end (flush batched collectives).
    virtual Status group_end() = 0;

    // --- Transport integration -----------------------------------------------

    /// Register a AXON transport endpoint that the plugin can use for
    /// custom communication patterns (e.g., fallback or hybrid).
    virtual Status register_transport(int peer_rank,
                                      std::shared_ptr<Endpoint> ep) {
        (void)peer_rank; (void)ep;
        return ErrorCode::kSuccess;  // optional
    }
};

// ---------------------------------------------------------------------------
// C factory function (exported from plugin shared libraries)
// ---------------------------------------------------------------------------

/// Every plugin .so must export this symbol.
using PluginFactoryFunc = CollectivePlugin* (*)();

#define ZEROKV_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))

/// Example usage in a plugin .so:
///
///   ZEROKV_PLUGIN_EXPORT zerokv::plugin::CollectivePlugin* zerokv_plugin_create() {
///       return new MyNcclPlugin();
///   }

// ---------------------------------------------------------------------------
// PluginRegistry – discovery and management
// ---------------------------------------------------------------------------

class PluginRegistry {
public:
    using Ptr = std::shared_ptr<PluginRegistry>;

    /// Get the global singleton registry.
    static PluginRegistry& instance();

    /// Manually register a plugin (for statically-linked plugins or tests).
    Status register_plugin(std::unique_ptr<CollectivePlugin> plugin);

    /// Load a plugin from a shared library path.
    Status load_plugin(const std::string& library_path);

    /// Discover plugins in a directory (searches for libzerokv_plugin_*.so).
    Status discover(const std::string& directory);

    /// Look up a plugin by name.
    [[nodiscard]] CollectivePlugin* find(const std::string& name) const;

    /// List all registered plugin names.
    [[nodiscard]] std::vector<std::string> list() const;

    /// Unload all plugins.
    void clear();

private:
    PluginRegistry() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace plugin
}  // namespace zerokv
