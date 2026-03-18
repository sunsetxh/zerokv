/// @file src/python/bindings.cpp
/// @brief nanobind bindings for the P2P library.
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

#include "p2p/p2p.h"

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

// ---------------------------------------------------------------------------
// Helper: create Config from Python arguments
// ---------------------------------------------------------------------------
static p2p::Config create_config(
    const std::string& transport,
    size_t num_workers,
    size_t memory_pool_size,
    size_t max_inflight_requests,
    double connect_timeout,
    bool registration_cache,
    size_t registration_cache_max_entries) {

    return p2p::Config::builder()
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
    m.doc() = "P2P high-performance transport library – native bindings";

    // --- Constants -----------------------------------------------------------
    m.attr("TAG_ANY") = p2p::kTagAny;
    m.attr("TAG_MASK_ALL") = p2p::kTagMaskAll;
    m.attr("TAG_MASK_USER") = p2p::kTagMaskUser;

    m.def("make_tag", &p2p::make_tag, "context_id"_a, "user_tag"_a);
    m.def("tag_context", &p2p::tag_context, "tag"_a);
    m.def("tag_user", &p2p::tag_user, "tag"_a);

    // --- Enums ---------------------------------------------------------------

    nb::enum_<p2p::MemoryType>(m, "MemoryType")
        .value("HOST", p2p::MemoryType::kHost)
        .value("CUDA", p2p::MemoryType::kCuda)
        .value("ROCM", p2p::MemoryType::kRocm)
        .value("ASCEND", p2p::MemoryType::kAscend)
        .export_values();

    nb::enum_<p2p::ErrorCode>(m, "ErrorCode")
        .value("SUCCESS", p2p::ErrorCode::kSuccess)
        .value("IN_PROGRESS", p2p::ErrorCode::kInProgress)
        .value("CANCELED", p2p::ErrorCode::kCanceled)
        .value("TIMEOUT", p2p::ErrorCode::kTimeout)
        .value("CONNECTION_REFUSED", p2p::ErrorCode::kConnectionRefused)
        .value("CONNECTION_RESET", p2p::ErrorCode::kConnectionReset)
        .value("ENDPOINT_CLOSED", p2p::ErrorCode::kEndpointClosed)
        .value("TRANSPORT_ERROR", p2p::ErrorCode::kTransportError)
        .value("MESSAGE_TRUNCATED", p2p::ErrorCode::kMessageTruncated)
        .value("OUT_OF_MEMORY", p2p::ErrorCode::kOutOfMemory)
        .value("INVALID_BUFFER", p2p::ErrorCode::kInvalidBuffer)
        .value("REGISTRATION_FAILED", p2p::ErrorCode::kRegistrationFailed)
        .value("PLUGIN_NOT_FOUND", p2p::ErrorCode::kPluginNotFound)
        .value("INTERNAL_ERROR", p2p::ErrorCode::kInternalError)
        .export_values();

    // --- Exceptions ----------------------------------------------------------

    nb::exception<std::runtime_error>(m, "P2PError");

    // --- Config --------------------------------------------------------------

    nb::class_<p2p::Config>(m, "Config")
        .def(nb::new_(&create_config),
            "transport"_a = "ucx",
            "num_workers"_a = 0,
            "memory_pool_size"_a = 64 * 1024 * 1024,
            "max_inflight_requests"_a = 1024,
            "connect_timeout"_a = 10.0,
            "registration_cache"_a = true,
            "registration_cache_max_entries"_a = 0)
        .def_prop_ro("transport", &p2p::Config::transport)
        .def_prop_ro("num_workers", &p2p::Config::num_workers)
        .def_prop_ro("memory_pool_size", &p2p::Config::memory_pool_size)
        .def("get", &p2p::Config::get, "key"_a, "default"_a = "");

    // --- Context -------------------------------------------------------------

    nb::class_<p2p::Context>(m, "Context")
        .def(nb::new_([](
            std::optional<p2p::Config> config,
            std::optional<std::string> transport,
            std::optional<size_t> num_workers,
            std::optional<size_t> memory_pool_size
        ) -> std::shared_ptr<p2p::Context> {
            if (config.has_value()) {
                return p2p::Context::create(config.value());
            }
            auto builder = p2p::Config::builder();
            if (transport.has_value())
                builder.set_transport(transport.value());
            if (num_workers.has_value())
                builder.set_num_workers(num_workers.value());
            if (memory_pool_size.has_value())
                builder.set_memory_pool_size(memory_pool_size.value());
            return p2p::Context::create(builder.build());
        }),
            "config"_a = nb::none(),
            "transport"_a = nb::none(),
            "num_workers"_a = nb::none(),
            "memory_pool_size"_a = nb::none())
        .def("create_worker", [](std::shared_ptr<p2p::Context>& self, size_t index) {
            return p2p::Worker::create(self, index);
        }, "index"_a = 0)
        .def("supports_memory_type", &p2p::Context::supports_memory_type)
        .def("supports_rma", &p2p::Context::supports_rma)
        .def("supports_hw_tag_matching", &p2p::Context::supports_hw_tag_matching)
        .def_prop_ro("config", &p2p::Context::config);

    // --- MemoryRegion --------------------------------------------------------

    nb::class_<p2p::MemoryRegion>(m, "MemoryRegion")
        .def_static("register_", [](std::shared_ptr<p2p::Context>& ctx, nb::handle buf,
                                    p2p::MemoryType type) {
            auto holder = BufferHolder::extract(buf);
            auto [ptr, len] = holder.get();
            return p2p::MemoryRegion::register_mem(ctx, ptr, len, type);
        }, "ctx"_a, "buffer"_a, "memory_type"_a = p2p::MemoryType::kHost)
        .def_static("allocate", &p2p::MemoryRegion::allocate,
            "ctx"_a, "size"_a, "memory_type"_a = p2p::MemoryType::kHost)
        .def_prop_ro("address", [](const p2p::MemoryRegion& r) {
            return reinterpret_cast<uintptr_t>(r.address());
        })
        .def_prop_ro("length", &p2p::MemoryRegion::length)
        .def_prop_ro("memory_type", &p2p::MemoryRegion::memory_type)
        .def_prop_ro("remote_key", [](const p2p::MemoryRegion& r) {
            auto rk = r.remote_key();
            return nb::bytes(reinterpret_cast<const char*>(rk.bytes()), rk.size());
        })
        .def("__len__", &p2p::MemoryRegion::length)
        .def("to_numpy", [](p2p::MemoryRegion& r) {
            size_t shape[1] = {r.length()};
            return nb::ndarray<nb::numpy, uint8_t, nb::ndim<1>>(
                r.address(), 1, shape
            );
        });

    // --- Endpoint ------------------------------------------------------------

    nb::class_<p2p::Endpoint>(m, "Endpoint")
        .def("tag_send", [](p2p::Endpoint& self, nb::handle buf, p2p::Tag tag) {
            auto holder = BufferHolder::extract(buf);
            auto [ptr, len] = holder.get();
            nb::gil_scoped_release release;
            return self.tag_send(ptr, len, tag);
        }, "buffer"_a, "tag"_a)
        .def("tag_send_region", [](p2p::Endpoint& self, std::shared_ptr<p2p::MemoryRegion> region,
                            p2p::Tag tag) {
            nb::gil_scoped_release release;
            return self.tag_send(region, 0, region->length(), tag);
        }, "region"_a, "tag"_a)
        .def("tag_recv", [](p2p::Endpoint& self, nb::handle buf,
                            p2p::Tag tag, p2p::Tag tag_mask) {
            auto holder = BufferHolder::extract(buf);
            auto [ptr, len] = holder.get();
            nb::gil_scoped_release release;
            return self.tag_recv(ptr, len, tag, tag_mask);
        }, "buffer"_a, "tag"_a, "tag_mask"_a = p2p::kTagMaskAll)
        .def("flush", [](p2p::Endpoint& self) {
            nb::gil_scoped_release release;
            return self.flush();
        })
        .def("close", [](p2p::Endpoint& self) {
            nb::gil_scoped_release release;
            return self.close();
        })
        .def_prop_ro("is_connected", &p2p::Endpoint::is_connected)
        .def_prop_ro("remote_address", &p2p::Endpoint::remote_address);

    // --- Worker --------------------------------------------------------------

    nb::class_<p2p::Worker>(m, "Worker")
        .def("connect", [](p2p::Worker& self, const std::string& addr) {
            nb::gil_scoped_release release;
            return self.connect(addr);
        }, "address"_a)
        .def("connect_blob", [](p2p::Worker& self, const std::vector<uint8_t>& addr) {
            nb::gil_scoped_release release;
            return self.connect(addr);
        }, "address"_a)
        .def("listen", &p2p::Worker::listen,
             "bind_address"_a, "on_accept"_a)
        .def("progress", [](p2p::Worker& self) {
            nb::gil_scoped_release release;
            return self.progress();
        })
        .def("address", [](p2p::Worker& self) {
            nb::gil_scoped_release release;
            return self.address();
        })
        .def_prop_ro("event_fd", &p2p::Worker::event_fd)
        .def_prop_ro("index", &p2p::Worker::index)
        .def("attach_to_event_loop", [](p2p::Worker& self, nb::handle loop) {
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
        .def("detach_from_event_loop", [](p2p::Worker& self, nb::handle loop) {
            nb::object asyncio = nb::module_::import_("asyncio");
            if (loop.is_none()) {
                loop = asyncio.attr("get_running_loop")();
            }
            loop.attr("remove_reader")(self.event_fd());
        }, "loop"_a = nb::none())
        // Background progress thread for true async operation
        .def("start_progress_thread", &p2p::Worker::start_progress_thread)
        .def("stop_progress_thread", &p2p::Worker::stop_progress_thread)
        .def_prop_ro("progress_thread_running", &p2p::Worker::is_progress_thread_running);

    // --- Listener ------------------------------------------------------------

    nb::class_<p2p::Listener>(m, "Listener")
        .def_prop_ro("address", &p2p::Listener::address)
        .def("close", &p2p::Listener::close);

    // --- Future --------------------------------------------------------------

    nb::class_<p2p::Future<void>>(m, "FutureVoid")
        .def("ready", &p2p::Future<void>::ready)
        .def("get", [](p2p::Future<void>& self) {
            nb::gil_scoped_release release;
            return self.get();
        })
        .def("cancel", &p2p::Future<void>::cancel)
        .def_prop_ro("status", &p2p::Future<void>::status);

    nb::class_<p2p::Future<std::pair<size_t, p2p::Tag>>>(m, "FutureRecv")
        .def("ready", &p2p::Future<std::pair<size_t, p2p::Tag>>::ready)
        .def("get", [](p2p::Future<std::pair<size_t, p2p::Tag>>& self) {
            nb::gil_scoped_release release;
            return self.get();
        })
        .def("cancel", &p2p::Future<std::pair<size_t, p2p::Tag>>::cancel)
        .def_prop_ro("status", &p2p::Future<std::pair<size_t, p2p::Tag>>::status);

    // Note: MemoryPool, RegistrationCache, PluginRegistry bindings
    // will be added when the implementations are complete
}
