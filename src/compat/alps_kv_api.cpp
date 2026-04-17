#include "yr/alps_kv_api.h"

#include "compat/alps_kv_channel.h"

#include <iostream>
#include <memory>
#include <string>

namespace {

std::unique_ptr<zerokv::compat::AlpsKvChannel> g_channel;

bool ShouldListenMode(const std::string& host) {
    return host == "listen" || host == "server" || host == "0.0.0.0";
}

std::string MakeAddress(const std::string& host, int port) {
    return host + ":" + std::to_string(port);
}

}  // namespace

namespace YR {

int SetClient(const char* host, int port, int connect_timeout_ms) {
    if (host == nullptr || port <= 0 || connect_timeout_ms <= 0) {
        std::cerr << "YR::SetClient: invalid args." << std::endl;
        return 0;
    }

    ShutdownClient();
    g_channel = std::make_unique<zerokv::compat::AlpsKvChannel>();

    const std::string host_value(host);
    const bool ok = ShouldListenMode(host_value)
        ? g_channel->Listen(MakeAddress("0.0.0.0", port), connect_timeout_ms)
        : g_channel->Connect(MakeAddress(host_value, port), connect_timeout_ms);
    if (!ok) {
        g_channel.reset();
        return 0;
    }
    return 1;
}

void ShutdownClient() {
    if (g_channel) {
        g_channel->Shutdown();
        g_channel.reset();
    }
}

std::string GetLocalAddress() {
    if (!g_channel) {
        return {};
    }
    return g_channel->local_address();
}

int WriteBytes(const void* data, size_t size, int tag, int index, int src, int dst) {
    if (!g_channel) {
        std::cerr << "YR::WriteBytes: channel not initialized, call YR::SetClient first." << std::endl;
        return 0;
    }
    return g_channel->WriteBytes(data, size, tag, index, src, dst) ? 1 : 0;
}

void ReadBytes(void* data, size_t size, int tag, int index, int src, int dst) {
    if (!g_channel) {
        std::cerr << "YR::ReadBytes: channel not initialized, call YR::SetClient first." << std::endl;
        return;
    }
    g_channel->ReadBytes(data, size, tag, index, src, dst);
}

void ReadBytesBatch(std::vector<void*>& data,
                    const std::vector<size_t>& sizes,
                    const std::vector<int>& tags,
                    const std::vector<int>& indices,
                    const std::vector<int>& srcs,
                    const std::vector<int>& dsts) {
    if (!g_channel) {
        std::cerr << "YR::ReadBytesBatch: channel not initialized, call YR::SetClient first." << std::endl;
        return;
    }
    g_channel->ReadBytesBatch(data, sizes, tags, indices, srcs, dsts);
}

}  // namespace YR
