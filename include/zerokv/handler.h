#ifndef ZEROKV_PROTOCOL_HANDLER_H
#define ZEROKV_PROTOCOL_HANDLER_H

#include "zerokv/protocol.h"
#include "zerokv/storage.h"
#include <memory>
#include <functional>
#include <vector>

namespace zerokv {

// Request handler callback types
using RequestHandler = std::function<std::vector<uint8_t>(
    const RequestHeader& header,
    const std::string& key,
    void* value)>;

// Protocol handler - processes protocol requests using storage engine
class ProtocolHandler {
public:
    ProtocolHandler();
    ~ProtocolHandler();

    // Initialize with storage engine
    void set_storage(std::shared_ptr<StorageEngine> storage);

    // Process a request and return response
    std::vector<uint8_t> handle_request(
        const std::vector<uint8_t>& request_data);

    // Process request header + key + value directly
    std::vector<uint8_t> handle_request(
        const RequestHeader& header,
        const std::string& key,
        void* value);

private:
    std::shared_ptr<StorageEngine> storage_;

    // Handle individual operations
    std::vector<uint8_t> handle_put(
        const RequestHeader& header,
        const std::string& key,
        void* value);

    std::vector<uint8_t> handle_get(
        const RequestHeader& header,
        const std::string& key,
        void* value);

    std::vector<uint8_t> handle_delete(
        const RequestHeader& header,
        const std::string& key,
        void* value);

    std::vector<uint8_t> handle_batch_put(
        const RequestHeader& header,
        const std::string& key,
        void* value);

    std::vector<uint8_t> handle_batch_get(
        const RequestHeader& header,
        const std::string& key,
        void* value);

    std::vector<uint8_t> handle_put_user_mem(
        const RequestHeader& header,
        const std::string& key,
        void* value);

    std::vector<uint8_t> handle_get_user_mem(
        const RequestHeader& header,
        const std::string& key,
        void* value);
};

} // namespace zerokv

#endif // ZEROKV_PROTOCOL_HANDLER_H
