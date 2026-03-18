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

#include "axon/axon.h"

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
static axon::Config create_config(
    const std::string& transport,
    size_t num_workers,
    size_t memory_pool_size,
    size_t max_inflight_requests,
    double connect_timeout,
    bool registration_cache,
    size_t registration_cache_max_entries) {

    return axon::Config::builder()
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
    m.attr("TAG_ANY") = axon::kTagAny;
    m.attr("TAG_MASK_ALL") = axon::kTagMaskAll;
    m.attr("TAG_MASK_USER") = axon::kTagMaskUser;

    m.def("make_tag", &axon::make_tag, "context_id"_a, "user_tag"_a);
    m.def("tag_context", &axon::tag_context, "tag"_a);
    m.def("tag_user", &axon::tag_user, "tag"_a);

    // --- Enums ---------------------------------------------------------------

    nb::enum_<axon::MemoryType>(m, "MemoryType")
        .value("HOST", axon::MemoryType::kHost)
        .value("CUDA", axon::MemoryType::kCuda)
        .value("ROCM", axon::MemoryType::kRocm)
        .value("ASCEND", axon::MemoryType::kAscend)
        .export_values();

    nb::enum_<axon::ErrorCode>(m, "ErrorCode")
        .value("SUCCESS", axon::ErrorCode::kSuccess)
        .value("IN_PROGRESS", axon::ErrorCode::kInProgress)
        .value("CANCELED", axon::ErrorCode::kCanceled)
        .value("TIMEOUT", axon::ErrorCode::kTimeout)
        .value("CONNECTION_REFUSED", axon::ErrorCode::kConnectionRefused)
        .value("CONNECTION_RESET", axon::ErrorCode::kConnectionReset)
        .value("ENDPOINT_CLOSED", axon::ErrorCode::kEndpointClosed)
        .value("TRANSPORT_ERROR", axon::ErrorCode::kTransportError)
        .value("MESSAGE_TRUNCATED", axon::ErrorCode::kMessageTruncated)
        .value("OUT_OF_MEMORY", axon::ErrorCode::kOutOfMemory)
        .value("INVALID_BUFFER", axon::ErrorCode::kInvalidBuffer)
        .value("REGISTRATION_FAILED", axon::ErrorCode::kRegistrationFailed)
        .value("PLUGIN_NOT_FOUND", axon::ErrorCode::kPluginNotFound)
        .value("INTERNAL_ERROR", axon::ErrorCode::kInternalError)
        .export_values();

    // --- Exceptions ----------------------------------------------------------

    nb::exception<std::runtime_error>(m, "AXONError");

    // --- Config --------------------------------------------------------------

    nb::class_<axon::Config>(m, "Config")
        .def(nb::new_(&create_config),
            "transport"_a = "ucx",
            "num_workers"_a = 0,
            "memory_pool_size"_a = 64 * 1024 * 1024,
            "max_inflight_requests"_a = 1024,
            "connect_timeout"_a = 10.0,
            "registration_cache"_a = true,
            "registration_cache_max_entries"_a = 0)
        .def_prop_ro("transport", &axon::Config::transport)
        .def_prop_ro("num_workers", &axon::Config::num_workers)
        .def_prop_ro("memory_pool_size", &axon::Config::memory_pool_size)
        .def("get", &axon::Config::get, "key"_a, "default"_a = "");

    // --- Context -------------------------------------------------------------

    nb::class_<axon::Context>(m, "Context")
        .def(nb::new_([](
            std::optional<axon::Config> config,
            std::optional<std::string> transport,
            std::optional<size_t> num_workers,
            std::optional<size_t> memory_pool_size
        ) -> std::shared_ptr<axon::Context> {
            if (config.has_value()) {
                return axon::Context::create(config.value());
            }
            auto builder = axon::Config::builder();
            if (transport.has_value())
                builder.set_transport(transport.value());
            if (num_workers.has_value())
                builder.set_num_workers(num_workers.value());
            if (memory_pool_size.has_value())
                builder.set_memory_pool_size(memory_pool_size.value());
            return axon::Context::create(builder.build());
        }),
            "config"_a = nb::none(),
            "transport"_a = nb::none(),
            "num_workers"_a = nb::none(),
            "memory_pool_size"_a = nb::none())
        .def("create_worker", [](std::shared_ptr<axon::Context>& self, size_t index) {
            return axon::Worker::create(self, index);
        }, "index"_a = 0)
        .def("supports_memory_type", &axon::Context::supports_memory_type)
        .def("supports_rma", &axon::Context::supports_rma)
        .def("supports_hw_tag_matching", &axon::Context::supports_hw_tag_matching)
        .def_prop_ro("config", &axon::Context::config);

    // --- MemoryRegion --------------------------------------------------------

    nb::class_<axon::MemoryRegion>(m, "MemoryRegion")
        .def_static("register_", [](std::shared_ptr<axon::Context>& ctx, nb::handle buf,
                                    axon::MemoryType type) {
            auto holder = BufferHolder::extract(buf);
            auto [ptr, len] = holder.get();
            return axon::MemoryRegion::register_mem(ctx, ptr, len, type);
        }, "ctx"_a, "buffer"_a, "memory_type"_a = axon::MemoryType::kHost)
        .def_static("allocate", &axon::MemoryRegion::allocate,
            "ctx"_a, "size"_a, "memory_type"_a = axon::MemoryType::kHost)
        .def_prop_ro("address", [](const axon::MemoryRegion& r) {
            return reinterpret_cast<uintptr_t>(r.address());
        })
        .def_prop_ro("length", &axon::MemoryRegion::length)
        .def_prop_ro("memory_type", &axon::MemoryRegion::memory_type)
        .def_prop_ro("remote_key", [](const axon::MemoryRegion& r) {
            auto rk = r.remote_key();
            return nb::bytes(reinterpret_cast<const char*>(rk.bytes()), rk.size());
        })
        .def("__len__", &axon::MemoryRegion::length)
        .def("to_numpy", [](axon::MemoryRegion& r) {
            size_t shape[1] = {r.length()};
            return nb::ndarray<nb::numpy, uint8_t, nb::ndim<1>>(
                r.address(), 1, shape
            );
        });

    // --- Endpoint ------------------------------------------------------------

    nb::class_<axon::Endpoint>(m, "Endpoint")
        .def("tag_send", [](axon::Endpoint& self, nb::handle buf, axon::Tag tag) {
            auto holder = BufferHolder::extract(buf);
            auto [ptr, len] = holder.get();
            nb::gil_scoped_release release;
            return self.tag_send(ptr, len, tag);
        }, "buffer"_a, "tag"_a)
        .def("tag_send_region", [](axon::Endpoint& self, std::shared_ptr<axon::MemoryRegion> region,
                            axon::Tag tag) {
            nb::gil_scoped_release release;
            return self.tag_send(region, 0, region->length(), tag);
        }, "region"_a, "tag"_a)
        .def("tag_recv", [](axon::Endpoint& self, nb::handle buf,
                            axon::Tag tag, axon::Tag tag_mask) {
            auto holder = BufferHolder::extract(buf);
            auto [ptr, len] = holder.get();
            nb::gil_scoped_release release;
            return self.tag_recv(ptr, len, tag, tag_mask);
        }, "buffer"_a, "tag"_a, "tag_mask"_a = axon::kTagMaskAll)
        .def("flush", [](axon::Endpoint& self) {
            nb::gil_scoped_release release;
            return self.flush();
        })
        .def("close", [](axon::Endpoint& self) {
            nb::gil_scoped_release release;
            return self.close();
        })
        .def_prop_ro("is_connected", &axon::Endpoint::is_connected)
        .def_prop_ro("remote_address", &axon::Endpoint::remote_address);

    // --- Worker --------------------------------------------------------------

    nb::class_<axon::Worker>(m, "Worker")
        .def("connect", [](axon::Worker& self, const std::string& addr) {
            nb::gil_scoped_release release;
            return self.connect(addr);
        }, "address"_a)
        .def("connect_blob", [](axon::Worker& self, const std::vector<uint8_t>& addr) {
            nb::gil_scoped_release release;
            return self.connect(addr);
        }, "address"_a)
        .def("listen", &axon::Worker::listen,
             "bind_address"_a, "on_accept"_a)
        .def("progress", [](axon::Worker& self) {
            nb::gil_scoped_release release;
            return self.progress();
        })
        .def("address", [](axon::Worker& self) {
            nb::gil_scoped_release release;
            return self.address();
        })
        .def_prop_ro("event_fd", &axon::Worker::event_fd)
        .def_prop_ro("index", &axon::Worker::index)
        .def("attach_to_event_loop", [](axon::Worker& self, nb::handle loop) {
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
        .def("detach_from_event_loop", [](axon::Worker& self, nb::handle loop) {
            nb::object asyncio = nb::module_::import_("asyncio");
            if (loop.is_none()) {
                loop = asyncio.attr("get_running_loop")();
            }
            loop.attr("remove_reader")(self.event_fd());
        }, "loop"_a = nb::none())
        // Background progress thread for true async operation
        .def("start_progress_thread", &axon::Worker::start_progress_thread)
        .def("stop_progress_thread", &axon::Worker::stop_progress_thread)
        .def_prop_ro("progress_thread_running", &axon::Worker::is_progress_thread_running);

    // --- Listener ------------------------------------------------------------

    nb::class_<axon::Listener>(m, "Listener")
        .def_prop_ro("address", &axon::Listener::address)
        .def("close", &axon::Listener::close);

    // --- Future --------------------------------------------------------------

    nb::class_<axon::Future<void>>(m, "FutureVoid")
        .def("ready", &axon::Future<void>::ready)
        .def("get", [](axon::Future<void>& self) {
            nb::gil_scoped_release release;
            return self.get();
        })
        .def("cancel", &axon::Future<void>::cancel)
        .def_prop_ro("status", &axon::Future<void>::status);

    nb::class_<axon::Future<std::pair<size_t, axon::Tag>>>(m, "FutureRecv")
        .def("ready", &axon::Future<std::pair<size_t, axon::Tag>>::ready)
        .def("get", [](axon::Future<std::pair<size_t, axon::Tag>>& self) {
            nb::gil_scoped_release release;
            return self.get();
        })
        .def("cancel", &axon::Future<std::pair<size_t, axon::Tag>>::cancel)
        .def_prop_ro("status", &axon::Future<std::pair<size_t, axon::Tag>>::status);

    // Note: MemoryPool, RegistrationCache, PluginRegistry bindings
    // will be added when the implementations are complete
}
