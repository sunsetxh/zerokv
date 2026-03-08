#include <pybind11/stl.h>
#include <pybind11/buffer_info.h>
#include "zerokv/client.h"
#include "zerokv/storage.h"

namespace py = pybind11;

namespace zerokv {

// Python wrapper for Client
class PyClient {
public:
    PyClient() : client_(std::make_unique<Client>()) {}

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
        if (status != Status::OK && status != Status::NOT_FOUND) {
            throw std::runtime_error("Failed to remove key");
        }
    }

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

    py::class_<zerokv::PyClient>(m, "ZeroKV")
        .def(py::init<>())
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
        .def("set_memory_type", &zerokv::PyClient::set_memory_type,
             "Set memory type for transport",
             py::arg("type"));

    // Memory type constants
    m.attr("MEMORY_CPU") = "cpu";
    m.attr("MEMORY_HUAWEI_NPU") = "huawei_npu";
    m.attr("MEMORY_NVIDIA_GPU") = "nvidia_gpu";
}
