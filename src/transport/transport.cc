#include "zerokv/transport.h"
#include "zerokv/storage.h"
#include "ucx_transport.h"
#include <memory>
#include <iostream>
#include <fstream>

namespace zerokv {

// Factory function to create transport instances
std::unique_ptr<Transport> create_transport(MemoryType type) {
    std::ofstream log("/tmp/zerokv_transport.log", std::ios::app);
    log << "[transport.cc] Creating UCXTransport..." << std::endl;
    log.flush();
    try {
        auto transport = std::make_unique<UCXTransport>();
        log << "[transport.cc] Transport created, ptr=" << transport.get() << std::endl;
        log.flush();
        log.close();
        return transport;
    } catch (const std::exception& e) {
        log << "[transport.cc] Exception: " << e.what() << std::endl;
        log.flush();
        log.close();
    } catch (...) {
        log << "[transport.cc] Unknown exception" << std::endl;
        log.flush();
        log.close();
    }
    return nullptr;
}

} // namespace zerokv
