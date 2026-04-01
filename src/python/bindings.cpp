/// @file src/python/bindings.cpp
/// @brief nanobind bindings for the AXON library.
///
/// Key design decisions:
///   - All blocking/async C++ calls release the GIL.
///   - Buffer-protocol objects (numpy, cupy, memoryview) are accepted,
///     extracting the raw pointer before releasing the GIL.
///   - Future<T> is made awaitable by implementing __await__ that yields
///     to the asyncio event loop.

#include <nanobind/nanobind.h>
#include <nanobind/nb_defs.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/function.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/optional.h>
#include <nanobind/ndarray.h>

#include "zerokv/zerokv.h"

namespace nb = nanobind;
using namespace nb::literals;

// ---------------------------------------------------------------------------
// Helper: extract pointer + length from any buffer-protocol object
// Keeps a reference to the Python object to prevent garbage collection
// ---------------------------------------------------------------------------
struct BufferHolder {
    PyObject* obj = nullptr;
    void* ptr = nullptr;
    size_t len = 0;

    BufferHolder() = default;
    BufferHolder(const BufferHolder&) = delete;
    BufferHolder& operator=(const BufferHolder&) = delete;
    BufferHolder(BufferHolder&& other) noexcept : obj(other.obj), ptr(other.ptr), len(other.len) {
        other.obj = nullptr;
    }
    BufferHolder& operator=(BufferHolder&& other) noexcept {
        if (this != &other) {
            if (obj) Py_DECREF(obj);
            obj = other.obj;
            ptr = other.ptr;
            len = other.len;
            other.obj = nullptr;
        }
        return *this;
    }

    static BufferHolder extract(nb::handle buf) {
        BufferHolder holder;
        holder.obj = buf.ptr();
        Py_INCREF(holder.obj);

        // Try bytes first
        if (PyBytes_Check(holder.obj)) {
            holder.len = PyBytes_GET_SIZE(holder.obj);
            holder.ptr = PyBytes_AS_STRING(holder.obj);
            return holder;
        }
        // Try bytearray
        if (PyByteArray_Check(holder.obj)) {
            holder.len = PyByteArray_GET_SIZE(holder.obj);
            holder.ptr = PyByteArray_AS_STRING(holder.obj);
            return holder;
        }
        // Try memoryview
        if (PyMemoryView_Check(holder.obj)) {
            Py_buffer* view = PyMemoryView_GET_BUFFER(holder.obj);
            holder.ptr = view->buf;
            holder.len = static_cast<size_t>(view->len);
            return holder;
        }
        // Try numpy array via nanobind ndarray
        try {
            auto arr = nb::cast<nb::ndarray<nb::numpy, const uint8_t, nb::ndim<1>>>(buf);
            holder.ptr = static_cast<void*>(const_cast<uint8_t*>(arr.data()));
            holder.len = static_cast<size_t>(arr.size());
            return holder;
        } catch (...) {
            // Fall through to error
        }

        Py_DECREF(holder.obj);
        holder.obj = nullptr;
        throw std::runtime_error("Unsupported buffer type. Use bytes, bytearray, memoryview, or numpy array.");
    }

    ~BufferHolder() {
        if (obj) {
            Py_DECREF(obj);
        }
    }

    std::pair<void*, size_t> get() const {
        return {ptr, len};
    }
};

static zerokv::RemoteKey extract_remote_key(nb::handle buf) {
    auto holder = BufferHolder::extract(buf);
    auto [ptr, len] = holder.get();
    zerokv::RemoteKey rkey;
    auto* bytes = static_cast<const uint8_t*>(ptr);
    rkey.data.assign(bytes, bytes + len);
    return rkey;
}

static nb::bytes to_bytes(const std::vector<uint8_t>& data) {
    return nb::bytes(reinterpret_cast<const char*>(data.data()), data.size());
}

static nb::bytes to_bytes(const std::vector<std::byte>& data) {
    return nb::bytes(reinterpret_cast<const char*>(data.data()), data.size());
}

// ---------------------------------------------------------------------------
// Helper: create Config from Python arguments
// ---------------------------------------------------------------------------
static zerokv::Config create_config(
    const std::string& transport,
    size_t num_workers,
    size_t memory_pool_size,
    size_t max_inflight_requests,
    double connect_timeout,
    bool registration_cache,
    size_t registration_cache_max_entries) {

    return zerokv::Config::builder()
        .set_transport(transport)
        .set_num_workers(num_workers)
        .set_memory_pool_size(memory_pool_size)
        .set_max_inflight_requests(max_inflight_requests)
        .set_connect_timeout(std::chrono::milliseconds(
            static_cast<int64_t>(connect_timeout * 1000)))
        .enable_registration_cache(registration_cache)
        .set_registration_cache_max_entries(registration_cache_max_entries)
        .build();
}

// ---------------------------------------------------------------------------
// Module definition
// ---------------------------------------------------------------------------
NB_MODULE(_core, m) {
    m.doc() = "AXON high-performance transport library – native bindings";

    // --- Constants -----------------------------------------------------------
    m.attr("TAG_ANY") = zerokv::kTagAny;
    m.attr("TAG_MASK_ALL") = zerokv::kTagMaskAll;
    m.attr("TAG_MASK_USER") = zerokv::kTagMaskUser;

    m.def("make_tag", &zerokv::make_tag, "context_id"_a, "user_tag"_a);
    m.def("tag_context", &zerokv::tag_context, "tag"_a);
    m.def("tag_user", &zerokv::tag_user, "tag"_a);

    // --- Enums ---------------------------------------------------------------

    nb::enum_<zerokv::MemoryType>(m, "MemoryType")
        .value("HOST", zerokv::MemoryType::kHost)
        .value("CUDA", zerokv::MemoryType::kCuda)
        .value("ROCM", zerokv::MemoryType::kRocm)
        .value("ASCEND", zerokv::MemoryType::kAscend)
        .export_values();

    nb::enum_<zerokv::ErrorCode>(m, "ErrorCode")
        .value("SUCCESS", zerokv::ErrorCode::kSuccess)
        .value("IN_PROGRESS", zerokv::ErrorCode::kInProgress)
        .value("CANCELED", zerokv::ErrorCode::kCanceled)
        .value("TIMEOUT", zerokv::ErrorCode::kTimeout)
        .value("CONNECTION_REFUSED", zerokv::ErrorCode::kConnectionRefused)
        .value("CONNECTION_RESET", zerokv::ErrorCode::kConnectionReset)
        .value("ENDPOINT_CLOSED", zerokv::ErrorCode::kEndpointClosed)
        .value("TRANSPORT_ERROR", zerokv::ErrorCode::kTransportError)
        .value("MESSAGE_TRUNCATED", zerokv::ErrorCode::kMessageTruncated)
        .value("OUT_OF_MEMORY", zerokv::ErrorCode::kOutOfMemory)
        .value("INVALID_BUFFER", zerokv::ErrorCode::kInvalidBuffer)
        .value("REGISTRATION_FAILED", zerokv::ErrorCode::kRegistrationFailed)
        .value("PLUGIN_NOT_FOUND", zerokv::ErrorCode::kPluginNotFound)
        .value("PLUGIN_INIT_FAILED", zerokv::ErrorCode::kPluginInitFailed)
        .value("INVALID_ARGUMENT", zerokv::ErrorCode::kInvalidArgument)
        .value("NOT_IMPLEMENTED", zerokv::ErrorCode::kNotImplemented)
        .value("INTERNAL_ERROR", zerokv::ErrorCode::kInternalError)
        .export_values();

    nb::class_<zerokv::Status>(m, "Status")
        .def(nb::init<>())
        .def(nb::init<zerokv::ErrorCode>())
        .def(nb::init<zerokv::ErrorCode, std::string>(), "code"_a, "message"_a)
        .def("ok", &zerokv::Status::ok)
        .def("in_progress", &zerokv::Status::in_progress)
        .def("throw_if_error", &zerokv::Status::throw_if_error)
        .def_prop_ro("code", &zerokv::Status::code)
        .def_prop_ro("message", &zerokv::Status::message)
        .def("__bool__", &zerokv::Status::ok)
        .def("__repr__", [](const zerokv::Status& s) {
            return "<zerokv.Status code=" + std::to_string(static_cast<int>(s.code())) +
                   " ok=" + std::string(s.ok() ? "True" : "False") +
                   " message='" + s.message() + "'>";
        });

    // --- Exceptions ----------------------------------------------------------

    nb::exception<std::runtime_error>(m, "AXONError");

    // --- Config --------------------------------------------------------------

    nb::class_<zerokv::Config>(m, "Config")
        .def(nb::new_(&create_config),
            "transport"_a = "ucx",
            "num_workers"_a = 0,
            "memory_pool_size"_a = 64 * 1024 * 1024,
            "max_inflight_requests"_a = 1024,
            "connect_timeout"_a = 10.0,
            "registration_cache"_a = true,
            "registration_cache_max_entries"_a = 0)
        .def_prop_ro("transport", &zerokv::Config::transport)
        .def_prop_ro("num_workers", &zerokv::Config::num_workers)
        .def_prop_ro("memory_pool_size", &zerokv::Config::memory_pool_size)
        .def("get", &zerokv::Config::get, "key"_a, "default"_a = "");

    // --- Context -------------------------------------------------------------

    nb::class_<zerokv::Context>(m, "Context")
        .def(nb::new_([](
            std::optional<zerokv::Config> config,
            std::optional<std::string> transport,
            std::optional<size_t> num_workers,
            std::optional<size_t> memory_pool_size
        ) -> std::shared_ptr<zerokv::Context> {
            if (config.has_value()) {
                return zerokv::Context::create(config.value());
            }
            auto builder = zerokv::Config::builder();
            if (transport.has_value())
                builder.set_transport(transport.value());
            if (num_workers.has_value())
                builder.set_num_workers(num_workers.value());
            if (memory_pool_size.has_value())
                builder.set_memory_pool_size(memory_pool_size.value());
            return zerokv::Context::create(builder.build());
        }),
            "config"_a = nb::none(),
            "transport"_a = nb::none(),
            "num_workers"_a = nb::none(),
            "memory_pool_size"_a = nb::none())
        .def("create_worker", [](std::shared_ptr<zerokv::Context>& self, size_t index) {
            return zerokv::Worker::create(self, index);
        }, "index"_a = 0)
        .def("supports_memory_type", &zerokv::Context::supports_memory_type)
        .def("supports_rma", &zerokv::Context::supports_rma)
        .def("supports_hw_tag_matching", &zerokv::Context::supports_hw_tag_matching)
        .def_prop_ro("config", &zerokv::Context::config);

    // --- MemoryRegion --------------------------------------------------------

    nb::class_<zerokv::MemoryRegion>(m, "MemoryRegion")
        .def_static("register_", [](std::shared_ptr<zerokv::Context>& ctx, nb::handle buf,
                                    zerokv::MemoryType type) {
            auto holder = BufferHolder::extract(buf);
            auto [ptr, len] = holder.get();
            return zerokv::MemoryRegion::register_mem(ctx, ptr, len, type);
        }, "ctx"_a, "buffer"_a, "memory_type"_a = zerokv::MemoryType::kHost)
        .def_static("allocate", &zerokv::MemoryRegion::allocate,
            "ctx"_a, "size"_a, "memory_type"_a = zerokv::MemoryType::kHost)
        .def_prop_ro("address", [](const zerokv::MemoryRegion& r) {
            return reinterpret_cast<uintptr_t>(r.address());
        })
        .def_prop_ro("length", &zerokv::MemoryRegion::length)
        .def_prop_ro("memory_type", &zerokv::MemoryRegion::memory_type)
        .def_prop_ro("remote_key", [](const zerokv::MemoryRegion& r) {
            auto rk = r.remote_key();
            return nb::bytes(reinterpret_cast<const char*>(rk.bytes()), rk.size());
        })
        .def("__len__", &zerokv::MemoryRegion::length)
        .def("to_numpy", [](zerokv::MemoryRegion& r) {
            size_t shape[1] = {r.length()};
            return nb::ndarray<nb::numpy, uint8_t, nb::ndim<1>>(
                r.address(), 1, shape
            );
        });

    // --- Endpoint ------------------------------------------------------------

    nb::class_<zerokv::Endpoint>(m, "Endpoint")
        .def("tag_send", [](zerokv::Endpoint& self, nb::handle buf, zerokv::Tag tag) {
            auto holder = BufferHolder::extract(buf);
            auto [ptr, len] = holder.get();
            nb::gil_scoped_release release;
            return self.tag_send(ptr, len, tag);
        }, "buffer"_a, "tag"_a)
        .def("tag_send_region", [](zerokv::Endpoint& self, std::shared_ptr<zerokv::MemoryRegion> region,
                            zerokv::Tag tag) {
            nb::gil_scoped_release release;
            return self.tag_send(region, 0, region->length(), tag);
        }, "region"_a, "tag"_a)
        .def("tag_recv", [](zerokv::Endpoint& self, nb::handle buf,
                            zerokv::Tag tag, zerokv::Tag tag_mask) {
            auto holder = BufferHolder::extract(buf);
            auto [ptr, len] = holder.get();
            nb::gil_scoped_release release;
            return self.tag_recv(ptr, len, tag, tag_mask);
        }, "buffer"_a, "tag"_a, "tag_mask"_a = zerokv::kTagMaskAll)
        .def("put", [](zerokv::Endpoint& self,
                       const std::shared_ptr<zerokv::MemoryRegion>& region,
                       uint64_t remote_addr,
                       nb::handle remote_key,
                       std::optional<size_t> length,
                       size_t local_offset) {
            auto rkey = extract_remote_key(remote_key);
            nb::gil_scoped_release release;
            if (length.has_value()) {
                return self.put(region, local_offset, remote_addr, rkey, *length);
            }
            return self.put(region, remote_addr, rkey);
        }, "region"_a, "remote_addr"_a, "remote_key"_a,
           "length"_a = nb::none(), "local_offset"_a = 0)
        .def("get", [](zerokv::Endpoint& self,
                       const std::shared_ptr<zerokv::MemoryRegion>& region,
                       uint64_t remote_addr,
                       nb::handle remote_key,
                       std::optional<size_t> length,
                       size_t local_offset) {
            auto rkey = extract_remote_key(remote_key);
            nb::gil_scoped_release release;
            if (length.has_value()) {
                return self.get(region, local_offset, remote_addr, rkey, *length);
            }
            return self.get(region, remote_addr, rkey);
        }, "region"_a, "remote_addr"_a, "remote_key"_a,
           "length"_a = nb::none(), "local_offset"_a = 0)
        .def("stream_send", [](zerokv::Endpoint& self, nb::handle buf) {
            auto holder = BufferHolder::extract(buf);
            auto [ptr, len] = holder.get();
            nb::gil_scoped_release release;
            return self.stream_send(ptr, len);
        }, "buffer"_a)
        .def("stream_send_region", [](zerokv::Endpoint& self,
                                      const std::shared_ptr<zerokv::MemoryRegion>& region) {
            nb::gil_scoped_release release;
            return self.stream_send(region);
        }, "region"_a)
        .def("stream_recv", [](zerokv::Endpoint& self, nb::handle buf) {
            auto holder = BufferHolder::extract(buf);
            auto [ptr, len] = holder.get();
            nb::gil_scoped_release release;
            return self.stream_recv(ptr, len);
        }, "buffer"_a)
        .def("stream_recv_region", [](zerokv::Endpoint& self,
                                      const std::shared_ptr<zerokv::MemoryRegion>& region) {
            nb::gil_scoped_release release;
            return self.stream_recv(region);
        }, "region"_a)
        .def("flush", [](zerokv::Endpoint& self) {
            nb::gil_scoped_release release;
            return self.flush();
        })
        .def("close", [](zerokv::Endpoint& self) {
            nb::gil_scoped_release release;
            return self.close();
        })
        .def_prop_ro("is_connected", &zerokv::Endpoint::is_connected)
        .def_prop_ro("remote_address", &zerokv::Endpoint::remote_address);

    // --- Worker --------------------------------------------------------------

    nb::class_<zerokv::Worker>(m, "Worker")
        .def("connect", [](zerokv::Worker& self, const std::string& addr) {
            nb::gil_scoped_release release;
            return self.connect(addr);
        }, "address"_a)
        .def("connect_blob", [](zerokv::Worker& self, const std::vector<uint8_t>& addr) {
            nb::gil_scoped_release release;
            return self.connect(addr);
        }, "address"_a)
        .def("listen", &zerokv::Worker::listen,
             "bind_address"_a, "on_accept"_a)
        .def("progress", [](zerokv::Worker& self) {
            nb::gil_scoped_release release;
            return self.progress();
        })
        .def("address", [](zerokv::Worker& self) {
            nb::gil_scoped_release release;
            return self.address();
        })
        .def_prop_ro("event_fd", &zerokv::Worker::event_fd)
        .def_prop_ro("index", &zerokv::Worker::index)
        .def("attach_to_event_loop", [](zerokv::Worker& self, nb::handle loop) {
            nb::object asyncio = nb::module_::import_("asyncio");
            if (loop.is_none()) {
                loop = asyncio.attr("get_running_loop")();
            }
            int fd = self.event_fd();
            if (fd < 0) {
                throw std::runtime_error("Worker does not support event fd");
            }
            nb::object progress_fn = nb::cpp_function([&self]() {
                self.progress();
            });
            loop.attr("add_reader")(fd, progress_fn);
        }, "loop"_a = nb::none())
        .def("detach_from_event_loop", [](zerokv::Worker& self, nb::handle loop) {
            nb::object asyncio = nb::module_::import_("asyncio");
            if (loop.is_none()) {
                loop = asyncio.attr("get_running_loop")();
            }
            loop.attr("remove_reader")(self.event_fd());
        }, "loop"_a = nb::none())
        // Background progress thread for true async operation
        .def("start_progress_thread", &zerokv::Worker::start_progress_thread)
        .def("stop_progress_thread", &zerokv::Worker::stop_progress_thread)
        .def_prop_ro("progress_thread_running", &zerokv::Worker::is_progress_thread_running);

    // --- KV -----------------------------------------------------------------

    nb::class_<zerokv::kv::KeyInfo>(m, "KeyInfo")
        .def_prop_ro("key", [](const zerokv::kv::KeyInfo& v) { return v.key; })
        .def_prop_ro("size", [](const zerokv::kv::KeyInfo& v) { return v.size; })
        .def_prop_ro("version", [](const zerokv::kv::KeyInfo& v) { return v.version; });

    nb::class_<zerokv::kv::FetchResult>(m, "FetchResult")
        .def_prop_ro("data", [](const zerokv::kv::FetchResult& v) { return to_bytes(v.data); })
        .def_prop_ro("owner_node_id", [](const zerokv::kv::FetchResult& v) { return v.owner_node_id; })
        .def_prop_ro("version", [](const zerokv::kv::FetchResult& v) { return v.version; });

    nb::class_<zerokv::kv::PublishMetrics>(m, "PublishMetrics")
        .def_prop_ro("total_us", [](const zerokv::kv::PublishMetrics& v) { return v.total_us; })
        .def_prop_ro("prepare_region_us", [](const zerokv::kv::PublishMetrics& v) { return v.prepare_region_us; })
        .def_prop_ro("pack_rkey_us", [](const zerokv::kv::PublishMetrics& v) { return v.pack_rkey_us; })
        .def_prop_ro("put_meta_rpc_us", [](const zerokv::kv::PublishMetrics& v) { return v.put_meta_rpc_us; })
        .def_prop_ro("ok", [](const zerokv::kv::PublishMetrics& v) { return v.ok; });

    nb::class_<zerokv::kv::FetchMetrics>(m, "FetchMetrics")
        .def_prop_ro("total_us", [](const zerokv::kv::FetchMetrics& v) { return v.total_us; })
        .def_prop_ro("local_buffer_prepare_us", [](const zerokv::kv::FetchMetrics& v) { return v.local_buffer_prepare_us; })
        .def_prop_ro("get_meta_rpc_us", [](const zerokv::kv::FetchMetrics& v) { return v.get_meta_rpc_us; })
        .def_prop_ro("peer_connect_us", [](const zerokv::kv::FetchMetrics& v) { return v.peer_connect_us; })
        .def_prop_ro("rkey_prepare_us", [](const zerokv::kv::FetchMetrics& v) { return v.rkey_prepare_us; })
        .def_prop_ro("get_submit_us", [](const zerokv::kv::FetchMetrics& v) { return v.get_submit_us; })
        .def_prop_ro("rdma_prepare_us", [](const zerokv::kv::FetchMetrics& v) { return v.rdma_prepare_us; })
        .def_prop_ro("rdma_get_us", [](const zerokv::kv::FetchMetrics& v) { return v.rdma_get_us; })
        .def_prop_ro("result_copy_us", [](const zerokv::kv::FetchMetrics& v) { return v.result_copy_us; })
        .def_prop_ro("ok", [](const zerokv::kv::FetchMetrics& v) { return v.ok; });

    nb::class_<zerokv::kv::PushMetrics>(m, "PushMetrics")
        .def_prop_ro("total_us", [](const zerokv::kv::PushMetrics& v) { return v.total_us; })
        .def_prop_ro("get_target_rpc_us", [](const zerokv::kv::PushMetrics& v) { return v.get_target_rpc_us; })
        .def_prop_ro("prepare_frame_us", [](const zerokv::kv::PushMetrics& v) { return v.prepare_frame_us; })
        .def_prop_ro("rdma_put_flush_us", [](const zerokv::kv::PushMetrics& v) { return v.rdma_put_flush_us; })
        .def_prop_ro("commit_rpc_us", [](const zerokv::kv::PushMetrics& v) { return v.commit_rpc_us; })
        .def_prop_ro("ok", [](const zerokv::kv::PushMetrics& v) { return v.ok; });

    nb::enum_<zerokv::kv::SubscriptionEventType>(m, "SubscriptionEventType")
        .value("PUBLISHED", zerokv::kv::SubscriptionEventType::kPublished)
        .value("UPDATED", zerokv::kv::SubscriptionEventType::kUpdated)
        .value("UNPUBLISHED", zerokv::kv::SubscriptionEventType::kUnpublished)
        .value("OWNER_LOST", zerokv::kv::SubscriptionEventType::kOwnerLost)
        .export_values();

    nb::class_<zerokv::kv::SubscriptionEvent>(m, "SubscriptionEvent")
        .def_prop_ro("type", [](const zerokv::kv::SubscriptionEvent& v) { return v.type; })
        .def_prop_ro("key", [](const zerokv::kv::SubscriptionEvent& v) { return v.key; })
        .def_prop_ro("owner_node_id", [](const zerokv::kv::SubscriptionEvent& v) { return v.owner_node_id; })
        .def_prop_ro("version", [](const zerokv::kv::SubscriptionEvent& v) { return v.version; });

    nb::class_<zerokv::kv::KVServer>(m, "KVServer")
        .def(nb::new_([](std::optional<zerokv::Config> config) {
            return zerokv::kv::KVServer::create(config.value_or(zerokv::Config{}));
        }), "config"_a = nb::none())
        .def("start", [](zerokv::kv::KVServer& self, const std::string& listen_addr) {
            nb::gil_scoped_release release;
            return self.start(zerokv::kv::ServerConfig{listen_addr});
        }, "listen_addr"_a)
        .def("stop", &zerokv::kv::KVServer::stop)
        .def_prop_ro("is_running", &zerokv::kv::KVServer::is_running)
        .def_prop_ro("address", &zerokv::kv::KVServer::address)
        .def("lookup", &zerokv::kv::KVServer::lookup, "key"_a)
        .def("list_keys", &zerokv::kv::KVServer::list_keys);

    nb::class_<zerokv::kv::KVNode>(m, "KVNode")
        .def(nb::new_([](std::optional<zerokv::Config> config) {
            return zerokv::kv::KVNode::create(config.value_or(zerokv::Config{}));
        }), "config"_a = nb::none())
        .def("start", [](zerokv::kv::KVNode& self,
                         const std::string& server_addr,
                         const std::string& local_data_addr,
                         const std::string& node_id) {
            nb::gil_scoped_release release;
            return self.start(zerokv::kv::NodeConfig{server_addr, local_data_addr, node_id});
        }, "server_addr"_a, "local_data_addr"_a, "node_id"_a = "")
        .def("stop", &zerokv::kv::KVNode::stop)
        .def_prop_ro("is_running", &zerokv::kv::KVNode::is_running)
        .def_prop_ro("node_id", &zerokv::kv::KVNode::node_id)
        .def_prop_ro("published_count", &zerokv::kv::KVNode::published_count)
        .def("last_publish_metrics", &zerokv::kv::KVNode::last_publish_metrics)
        .def("last_fetch_metrics", &zerokv::kv::KVNode::last_fetch_metrics)
        .def("last_push_metrics", &zerokv::kv::KVNode::last_push_metrics)
        .def("drain_subscription_events", &zerokv::kv::KVNode::drain_subscription_events)
        .def("publish", [](zerokv::kv::KVNode& self, const std::string& key, nb::handle buf) {
            auto holder = BufferHolder::extract(buf);
            auto [ptr, len] = holder.get();
            nb::gil_scoped_release release;
            return self.publish(key, ptr, len);
        }, "key"_a, "buffer"_a)
        .def("publish_region", [](zerokv::kv::KVNode& self,
                                  const std::string& key,
                                  const std::shared_ptr<zerokv::MemoryRegion>& region,
                                  std::optional<size_t> size) {
            nb::gil_scoped_release release;
            return self.publish_region(key, region, size.value_or(region->length()));
        }, "key"_a, "region"_a, "size"_a = nb::none())
        .def("fetch", [](zerokv::kv::KVNode& self, const std::string& key) {
            nb::gil_scoped_release release;
            return self.fetch(key);
        }, "key"_a)
        .def("fetch_to", [](zerokv::kv::KVNode& self,
                            const std::string& key,
                            const std::shared_ptr<zerokv::MemoryRegion>& region,
                            size_t length,
                            size_t local_offset) {
            nb::gil_scoped_release release;
            return self.fetch_to(key, region, length, local_offset);
        }, "key"_a, "region"_a, "length"_a, "local_offset"_a = 0)
        .def("push", [](zerokv::kv::KVNode& self,
                        const std::string& target_node_id,
                        const std::string& key,
                        nb::handle buf) {
            auto holder = BufferHolder::extract(buf);
            auto [ptr, len] = holder.get();
            nb::gil_scoped_release release;
            return self.push(target_node_id, key, ptr, len);
        }, "target_node_id"_a, "key"_a, "buffer"_a)
        .def("subscribe", [](zerokv::kv::KVNode& self, const std::string& key) {
            nb::gil_scoped_release release;
            return self.subscribe(key);
        }, "key"_a)
        .def("unsubscribe", [](zerokv::kv::KVNode& self, const std::string& key) {
            nb::gil_scoped_release release;
            return self.unsubscribe(key);
        }, "key"_a)
        .def("unpublish", [](zerokv::kv::KVNode& self, const std::string& key) {
            nb::gil_scoped_release release;
            return self.unpublish(key);
        }, "key"_a);

    // --- Listener ------------------------------------------------------------

    nb::class_<zerokv::Listener>(m, "Listener")
        .def_prop_ro("address", &zerokv::Listener::address)
        .def("close", &zerokv::Listener::close);

    // --- Future --------------------------------------------------------------

    nb::class_<zerokv::Future<void>>(m, "FutureVoid")
        .def("ready", &zerokv::Future<void>::ready)
        .def("get", [](zerokv::Future<void>& self) {
            nb::gil_scoped_release release;
            return self.get();
        })
        .def("cancel", &zerokv::Future<void>::cancel)
        .def_prop_ro("status", &zerokv::Future<void>::status);

    nb::class_<zerokv::Future<size_t>>(m, "FutureSize")
        .def("ready", &zerokv::Future<size_t>::ready)
        .def("get", [](zerokv::Future<size_t>& self) {
            nb::gil_scoped_release release;
            return self.get();
        })
        .def("cancel", &zerokv::Future<size_t>::cancel)
        .def_prop_ro("status", &zerokv::Future<size_t>::status);

    nb::class_<zerokv::Future<std::pair<size_t, zerokv::Tag>>>(m, "FutureRecv")
        .def("ready", &zerokv::Future<std::pair<size_t, zerokv::Tag>>::ready)
        .def("get", [](zerokv::Future<std::pair<size_t, zerokv::Tag>>& self) {
            nb::gil_scoped_release release;
            return self.get();
        })
        .def("cancel", &zerokv::Future<std::pair<size_t, zerokv::Tag>>::cancel)
        .def_prop_ro("status", &zerokv::Future<std::pair<size_t, zerokv::Tag>>::status);

    nb::class_<zerokv::Future<uint64_t>>(m, "FutureU64")
        .def("ready", &zerokv::Future<uint64_t>::ready)
        .def("get", [](zerokv::Future<uint64_t>& self) {
            nb::gil_scoped_release release;
            return self.get();
        })
        .def("cancel", &zerokv::Future<uint64_t>::cancel)
        .def_prop_ro("status", &zerokv::Future<uint64_t>::status);

    nb::class_<zerokv::Future<std::shared_ptr<zerokv::Endpoint>>>(m, "FutureEndpoint")
        .def("ready", &zerokv::Future<std::shared_ptr<zerokv::Endpoint>>::ready)
        .def("get", [](zerokv::Future<std::shared_ptr<zerokv::Endpoint>>& self) {
            nb::gil_scoped_release release;
            return self.get();
        })
        .def("cancel", &zerokv::Future<std::shared_ptr<zerokv::Endpoint>>::cancel)
        .def_prop_ro("status", &zerokv::Future<std::shared_ptr<zerokv::Endpoint>>::status);

    nb::class_<zerokv::Future<zerokv::kv::FetchResult>>(m, "FutureFetch")
        .def("ready", &zerokv::Future<zerokv::kv::FetchResult>::ready)
        .def("get", [](zerokv::Future<zerokv::kv::FetchResult>& self) {
            nb::gil_scoped_release release;
            return self.get();
        })
        .def("cancel", &zerokv::Future<zerokv::kv::FetchResult>::cancel)
        .def_prop_ro("status", &zerokv::Future<zerokv::kv::FetchResult>::status);

    // Note: MemoryPool, RegistrationCache, PluginRegistry bindings
    // will be added when the implementations are complete
}
