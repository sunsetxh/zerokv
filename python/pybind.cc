#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/buffer_info.h>
#include "zerokv/client.h"
#include "zerokv/storage.h"

namespace py = pybind11;

namespace zerokv {

// Python wrapper for Client with context manager support
class PyClient {
public:
    PyClient() : client_(std::make_unique<Client>()) {}

    // Context manager support
    void enter() {
        // Auto-connect if servers are configured
    }

    void exit(py::object exc_type, py::object exc_val, py::object exc_tb) {
        (void)exc_type;
        (void)exc_val;
        (void)exc_tb;
        disconnect();
    }

    void connect(const std::vector<std::string>& servers) {
        Status status = client_->connect(servers);
        if (status != Status::OK) {
            throw std::runtime_error("Failed to connect to ZeroKV cluster");
        }
    }

    void disconnect() {
        client_->disconnect();
    }

    void put(const std::string& key, const std::string& value) {
        Status status = client_->put(key, value.data(), value.size());
        if (status != Status::OK) {
            throw std::runtime_error("Failed to put key-value");
        }
    }

    std::string get(const std::string& key) {
        std::string value;
        Status status = client_->get(key, &value);
        if (status == Status::NOT_FOUND) {
            throw py::key_error(key);
        }
        if (status != Status::OK) {
            throw std::runtime_error("Failed to get key");
        }
        return value;
    }

    void remove(const std::string& key) {
        Status status = client_->remove(key);
        // NOT_FOUND is OK - key doesn't exist
        if (status != Status::OK && status != Status::NOT_FOUND) {
            throw std::runtime_error("Failed to remove key");
        }
    }

    // Batch operations
    void batch_put(const std::vector<std::pair<std::string, std::string>>& items) {
        for (const auto& item : items) {
            put(item.first, item.second);
        }
    }

    std::vector<std::string> batch_get(const std::vector<std::string>& keys) {
        std::vector<std::string> results;
        for (const auto& key : keys) {
            try {
                results.push_back(get(key));
            } catch (const py::key_error&) {
                results.push_back("");  // Return empty for not found
            }
        }
        return results;
    }

    // Get client for direct access
    Client* client() { return client_.get(); }

    void set_memory_type(const std::string& type) {
        if (type == "cpu") {
            client_->set_memory_type(MemoryType::CPU);
        } else if (type == "huawei_npu") {
            client_->set_memory_type(MemoryType::HUAWEI_NPU);
        } else if (type == "nvidia_gpu") {
            client_->set_memory_type(MemoryType::NVIDIA_GPU);
        } else {
            throw std::runtime_error("Unknown memory type: " + type);
        }
    }

private:
    std::unique_ptr<Client> client_;
};

} // namespace zerokv

PYBIND11_MODULE(_zerokv, m) {
    m.doc() = "ZeroKV - High-performance distributed KV store";

    // Register exception translations
    py::register_exception_translator([](std::exception_ptr p) {
        try {
            if (p) std::rethrow_exception(p);
        } catch (const py::key_error& e) {
            PyErr_SetString(PyExc_KeyError, e.what());
        }
    });

    // Main Client class
    // Use "Client" as primary name, also expose as "ZeroKV" for backwards compatibility
    py::class_<zerokv::PyClient>(m, "Client")
        .def(py::init<>())
        .def("__enter__", &zerokv::PyClient::enter)
        .def("__exit__", &zerokv::PyClient::exit)
        .def("connect", &zerokv::PyClient::connect,
             "Connect to ZeroKV cluster",
             py::arg("servers"))
        .def("disconnect", &zerokv::PyClient::disconnect,
             "Disconnect from cluster")
        .def("put", &zerokv::PyClient::put,
             "Put key-value pair",
             py::arg("key"), py::arg("value"))
        .def("get", &zerokv::PyClient::get,
             "Get value by key",
             py::arg("key"))
        .def("remove", &zerokv::PyClient::remove,
             "Remove key",
             py::arg("key"))
        .def("batch_put", &zerokv::PyClient::batch_put,
             "Batch put key-value pairs",
             py::arg("items"))
        .def("batch_get", &zerokv::PyClient::batch_get,
             "Batch get values by keys",
             py::arg("keys"))
        .def("set_memory_type", &zerokv::PyClient::set_memory_type,
             "Set memory type for transport",
             py::arg("type"));

    // Memory type constants
    m.attr("MEMORY_CPU") = "cpu";
    m.attr("MEMORY_HUAWEI_NPU") = "huawei_npu";
    m.attr("MEMORY_NVIDIA_GPU") = "nvidia_gpu";

    // Version info
    m.attr("__version__") = "0.1.0";

    // Add backwards compatible alias
    m.attr("ZeroKV") = m.attr("Client");
}
