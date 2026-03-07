# Public API Contracts

## Version: 1.0.0-MVP

This document defines the stable public API contracts for the P2P Transport Library. All APIs follow semantic versioning.

---

## Core Headers

```cpp
#include <p2p/p2p.h>        // Umbrella header
// Or individual headers:
#include <p2p/common.h>     // ErrorCode, Status, Tag, MemoryType
#include <p2p/config.h>     // Config, Context
#include <p2p/worker.h>     // Worker, Listener
#include <p2p/endpoint.h>   // Endpoint
#include <p2p/memory.h>     // MemoryRegion
#include <p2p/future.h>     // Future<T>, Request
```

---

## Contract: Error Handling

### ErrorCode Enum

```cpp
enum class ErrorCode : int {
    kSuccess             = 0,
    kInProgress          = 1,
    kCanceled            = 2,
    kTimeout             = 3,
    // Connection errors (100-199)
    kConnectionRefused   = 100,
    kConnectionReset     = 101,
    kConnectionAborted   = 102,
    // Transport errors (200-299)
    kTransportError      = 200,
    kNoResources         = 201,
    // Memory errors (300-399)
    kOutOfMemory         = 300,
    kRegistrationFailed  = 302,
    kInvalidMemory       = 303,
    // Generic errors (900-999)
    kInvalidArgument     = 900,
    kNotImplemented      = 901,
    kInternalError       = 999,
};
```

### Status Class

```cpp
class Status {
public:
    Status() noexcept;
    Status(ErrorCode code, std::string message = {});

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] bool in_progress() const noexcept;
    [[nodiscard]] ErrorCode code() const noexcept;
    [[nodiscard]] const std::string& message() const noexcept;
    [[nodiscard]] std::error_code error_code() const noexcept;

    void throw_if_error() const;
};
```

**Contract**:
- Default construction creates `kSuccess`
- `ok()` returns true only for `kSuccess`
- `in_progress()` returns true only for `kInProgress`
- `throw_if_error()` throws `std::system_error` if not `ok()` and not `in_progress()`

---

## Contract: Configuration

### Config::Builder

```cpp
class Config {
public:
    class Builder {
    public:
        Builder();

        Builder& set_transport(std::string name);
        Builder& set_num_workers(size_t n);
        Builder& set_memory_pool_size(size_t bytes);
        Builder& set_max_inflight_requests(size_t n);
        Builder& set_connect_timeout(std::chrono::milliseconds ms);
        Builder& enable_registration_cache(bool enable = true);
        Builder& set_registration_cache_max_entries(size_t n);
        Builder& set(std::string key, std::string value);
        Builder& from_env();

        Config build();
    };

    [[nodiscard]] const std::string& transport() const noexcept;
    [[nodiscard]] size_t num_workers() const noexcept;
    [[nodiscard]] size_t memory_pool_size() const noexcept;
    [[nodiscard]] size_t max_inflight_requests() const noexcept;
    [[nodiscard]] std::chrono::milliseconds connect_timeout() const noexcept;
    [[nodiscard]] bool registration_cache_enabled() const noexcept;
    [[nodiscard]] size_t registration_cache_max_entries() const noexcept;
    [[nodiscard]] const std::string& get(const std::string& key,
                                          const std::string& default_val = {}) const;

    static Builder builder();
};
```

**Contract**:
- Builder methods return `Builder&` for chaining
- `build()` transfers ownership to immutable `Config`
- `from_env()` reads `P2P_*` prefixed env vars

### Context

```cpp
class Context : public std::enable_shared_from_this<Context> {
public:
    using Ptr = std::shared_ptr<Context>;

    static Ptr create(const Config& config = Config::builder().build());

    [[nodiscard]] void* native_handle() const noexcept;
    [[nodiscard]] bool supports_rma() const noexcept;
    [[nodiscard]] bool supports_memory_type(MemoryType type) const noexcept;

private:
    Context() = default;
};
```

**Contract**:
- Factory method `create()` returns non-null `Ptr`
- `native_handle()` returns valid `ucp_context_h`
- Thread-safe: multiple threads may share one Context

---

## Contract: Worker

```cpp
class Worker : public std::enable_shared_from_this<Worker> {
public:
    using Ptr = std::shared_ptr<Worker>;

    static Ptr create(const Context::Ptr& ctx, size_t index = 0);

    // Progress
    bool progress() noexcept;
    bool wait(std::chrono::milliseconds timeout);
    void run_until(std::function<bool()> pred);
    void run();
    void stop() noexcept;

    // Connections
    Future<Endpoint::Ptr> connect(const std::string& address);
    Listener::Ptr listen(const std::string& address,
                        std::function<void(Endpoint::Ptr)> on_accept);

    // Utilities
    [[nodiscard]] int event_fd() const noexcept;
    [[nodiscard]] std::vector<uint8_t> worker_address() const;
    [[nodiscard]] void* native_handle() const noexcept;
    [[nodiscard]] std::shared_ptr<Context> context() const noexcept;

private:
    Worker() = default;
};
```

**Contract**:
- Thread-affine: all methods must be called from creating thread
- `progress()` returns true if work was done
- `event_fd()` returns valid file descriptor for epoll
- `worker_address()` returns serializable address for connection exchange

---

## Contract: Endpoint

```cpp
class Endpoint : public std::enable_shared_from_this<Endpoint> {
public:
    using Ptr = std::shared_ptr<Endpoint>;

    // Tag messaging (two-sided)
    Future<void> tag_send(const void* buffer, size_t length, Tag tag);
    Future<void> tag_send(const MemoryRegion::Ptr& region, size_t offset, size_t length, Tag tag);
    Future<std::pair<size_t, Tag>> tag_recv(void* buffer, size_t length,
                                             Tag tag, Tag tag_mask = kTagMaskAll);

    // RDMA (one-sided)
    Future<void> put(const MemoryRegion::Ptr& local, size_t local_offset,
                     uint64_t remote_addr, const RemoteKey& rkey, size_t length);
    Future<void> get(const MemoryRegion::Ptr& local, size_t local_offset,
                     uint64_t remote_addr, const RemoteKey& rkey, size_t length);
    Future<void> flush();

    // Atomic
    Future<uint64_t> atomic_fadd(const MemoryRegion::Ptr& local, size_t offset,
                                  uint64_t value, uint64_t remote_addr,
                                  const RemoteKey& rkey);

    // Lifecycle
    Future<void> close();
    [[nodiscard]] bool is_connected() const noexcept;

    // Accessors
    [[nodiscard]] std::shared_ptr<Worker> worker() const noexcept;
    [[nodiscard]] void* native_handle() const noexcept;

private:
    Endpoint() = default;
};
```

**Contract**:
- All operations return `Future<T>` (non-blocking)
- Must be used from same thread as creating Worker
- `close()` is asynchronous; Endpoint invalid after close returns

---

## Contract: Listener

```cpp
class Listener {
public:
    using Ptr = std::shared_ptr<Listener>;

    [[nodiscard]] std::string address() const;
    [[nodiscard]] void* native_handle() const noexcept;
    [[nodiscard]] std::shared_ptr<Worker> worker() const noexcept;

    void close();

private:
    Listener() = default;
};
```

---

## Contract: Memory Region

```cpp
class MemoryRegion {
public:
    using Ptr = std::shared_ptr<MemoryRegion>;

    // Register existing buffer
    static Ptr register_mem(const Context::Ptr& ctx, void* address, size_t length,
                           MemoryType type = MemoryType::kHost);

    // Allocate + register
    static Ptr allocate(const Context::Ptr& ctx, size_t length,
                       MemoryType type = MemoryType::kHost);

    [[nodiscard]] void* address() const noexcept;
    [[nodiscard]] size_t length() const noexcept;
    [[nodiscard]] MemoryType memory_type() const noexcept;
    [[nodiscard]] RemoteKey remote_key() const;
    [[nodiscard]] void* native_handle() const noexcept;

    template <typename T>
    [[nodiscard]] std::span<T> as_span() const;

    void deregister();

private:
    MemoryRegion() = default;
};
```

**Contract**:
- `address()` and `length()` valid until `deregister()` called
- `remote_key()` serializable for transfer to peer
- Thread-safe: multiple threads may read same region

---

## Contract: Future

```cpp
template <typename T>
class Future {
public:
    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;
    Future(Future&&) noexcept;
    Future& operator=(Future&&) noexcept;

    // Non-blocking
    [[nodiscard]] bool ready() const noexcept;

    // Blocking
    [[nodiscard]] T get();
    [[nodiscard]] std::optional<T> get(std::chrono::milliseconds timeout);

    // Status
    [[nodiscard]] Status status() const noexcept;
    void cancel();

    // Chaining
    template <typename Func>
    auto then(Func&& func) -> Future<std::invoke_result_t<Func, T>>;

    // Callback
    using Callback = std::function<void(Status, T)>;
    void on_complete(Callback callback);

    [[nodiscard]] Request::Ptr request() const noexcept;

private:
    explicit Future(Request::Ptr request);
};

// Specializations
extern template class Future<void>;
extern template class Future<size_t>;
extern template class Future<std::pair<size_t, Tag>>;
```

**Contract**:
- Move-only, non-copyable
- `ready()` returns true if complete (success or failure)
- `get()` blocks until complete, returns value or throws
- `then()` creates new Future chaining result
- `on_complete()` callback invoked exactly once

---

## Contract: Request

```cpp
class Request {
public:
    using Ptr = std::shared_ptr<Request>;

    [[nodiscard]] bool is_complete() const noexcept;
    [[nodiscard]] Status status() const noexcept;
    [[nodiscard]] Status wait(std::chrono::milliseconds timeout);
    [[nodiscard]] size_t bytes_transferred() const noexcept;
    [[nodiscard]] void* native_handle() const noexcept;

    void cancel();

private:
    Request() = default;
};
```

---

## Contract: Tags

```cpp
using Tag = uint64_t;

constexpr Tag kTagAny = UINT64_MAX;
constexpr Tag kTagMaskAll = UINT64_MAX;
constexpr Tag kTagMaskUser = 0xFFFFFFFF;

[[nodiscard]] Tag make_tag(uint32_t context, uint32_t user) noexcept;
[[nodiscard]] uint32_t tag_context(Tag tag) noexcept;
[[nodiscard]] uint32_t tag_user(Tag tag) noexcept;
```

---

## Contract: Memory Type

```cpp
enum class MemoryType : int {
    kHost   = 0,  // System RAM
    kCuda   = 1,  // NVIDIA GPU
    kROCm   = 2,  // AMD GPU
    kAscend = 3,  // Huawei Ascend
};
```

---

## Contract: Plugin Interface

```cpp
namespace plugin {

class CollectivePlugin {
public:
    virtual ~CollectivePlugin() = default;

    [[nodiscard]] virtual const char* name() const noexcept = 0;
    [[nodiscard]] virtual const char* version() const noexcept = 0;
    [[nodiscard]] virtual std::vector<MemoryType> supported_memory_types() const = 0;

    virtual Status init(const Context::Ptr& ctx) = 0;
    virtual Status shutdown() = 0;

    // ... collective operations
};

class PluginRegistry {
public:
    static PluginRegistry& instance();
    Status register_plugin(std::unique_ptr<CollectivePlugin> plugin);
    Status load_plugin(const std::string& path);
    [[nodiscard]] CollectivePlugin* find(const std::string& name) const;
    [[nodiscard]] std::vector<std::string> list() const;
};

}  // namespace plugin
```

---

## Backward Compatibility

MVP version 1.0.0:

- **Stable APIs**: Config, Context, Worker, Endpoint, MemoryRegion, Future<T>, Status
- **Experimental**: Plugin interface (may change in 2.0)
- **Removed in MVP**: Python bindings (deferred to Phase 2)

---

## Error Handling Rules

1. Synchronous functions return `Status` or throw
2. Asynchronous functions return `Future<T>` with `Status` accessible via `.status()`
3. `throw_if_error()` provided for exception-based code
4. All Status errors map to `std::error_code` for interoperability
