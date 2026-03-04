#include "zerokv/handler.h"
#include "zerokv/storage.h"
#include <cstring>
#include <iostream>

namespace zerokv {

ProtocolHandler::ProtocolHandler()
    : storage_(nullptr) {
}

ProtocolHandler::~ProtocolHandler() {
}

void ProtocolHandler::set_storage(std::shared_ptr<StorageEngine> storage) {
    storage_ = storage;
}

std::vector<uint8_t> ProtocolHandler::handle_request(
    const std::vector<uint8_t>& request_data) {

    // Decode request
    RequestHeader header;
    std::string key;
    void* value = nullptr;

    if (!ProtocolCodec::decode_request(request_data, header, key, value)) {
        // Return error response
        return ProtocolCodec::encode_response(Status::ERROR, header.request_id);
    }

    // Handle the request
    return handle_request(header, key, value);
}

std::vector<uint8_t> ProtocolHandler::handle_request(
    const RequestHeader& header,
    const std::string& key,
    void* value) {

    switch (static_cast<OpCode>(header.opcode)) {
        case OpCode::PUT:
            return handle_put(header, key, value);
        case OpCode::GET:
            return handle_get(header, key, value);
        case OpCode::DELETE:
            return handle_delete(header, key, value);
        case OpCode::BATCH_PUT:
            return handle_batch_put(header, key, value);
        case OpCode::BATCH_GET:
            return handle_batch_get(header, key, value);
        case OpCode::PUT_USER_MEM:
            return handle_put_user_mem(header, key, value);
        case OpCode::GET_USER_MEM:
            return handle_get_user_mem(header, key, value);
        default:
            return ProtocolCodec::encode_response(Status::ERROR, header.request_id);
    }
}

std::vector<uint8_t> ProtocolHandler::handle_put(
    const RequestHeader& header,
    const std::string& key,
    void* value) {

    if (!storage_) {
        return ProtocolCodec::encode_response(Status::ERROR, header.request_id);
    }

    Status status = storage_->put(key, value, header.value_len);
    return ProtocolCodec::encode_response(status, header.request_id);
}

std::vector<uint8_t> ProtocolHandler::handle_get(
    const RequestHeader& header,
    const std::string& key,
    void* value) {

    if (!storage_) {
        return ProtocolCodec::encode_response(Status::ERROR, header.request_id);
    }

    // Allocate buffer for value
    std::vector<uint8_t> buffer(header.value_len > 0 ? header.value_len : 4096);
    size_t size = buffer.size();

    Status status = storage_->get(key, buffer.data(), &size);

    if (status == Status::OK) {
        return ProtocolCodec::encode_response(status, header.request_id,
                                              buffer.data(), size);
    }

    return ProtocolCodec::encode_response(status, header.request_id);
}

std::vector<uint8_t> ProtocolHandler::handle_delete(
    const RequestHeader& header,
    const std::string& key,
    void* value) {

    if (!storage_) {
        return ProtocolCodec::encode_response(Status::ERROR, header.request_id);
    }

    Status status = storage_->delete_key(key);
    return ProtocolCodec::encode_response(status, header.request_id);
}

std::vector<uint8_t> ProtocolHandler::handle_batch_put(
    const RequestHeader& header,
    const std::string& key,
    void* value) {

    // Batch format: key1\0value1\0key2\0value2\0...
    if (!storage_ || !value || header.value_len == 0) {
        return ProtocolCodec::encode_response(Status::ERROR, header.request_id);
    }

    const char* data = static_cast<const char*>(value);
    size_t offset = 0;
    size_t total = header.value_len;
    int count = 0;

    while (offset < total) {
        // Read key
        size_t key_len = strlen(data + offset);
        if (key_len == 0 || offset + key_len + 1 >= total) break;

        std::string item_key(data + offset, key_len);
        offset += key_len + 1;

        // Read value
        if (offset >= total) break;
        size_t value_len = total - offset - 1;
        std::string item_value(data + offset, value_len);

        Status status = storage_->put(item_key, item_value.data(), item_value.size());
        if (status != Status::OK) {
            return ProtocolCodec::encode_response(status, header.request_id);
        }

        offset += value_len + 1;
        count++;
    }

    // Return count as value
    std::vector<uint8_t> response;
    ResponseHeader resp_header;
    resp_header.status = static_cast<uint8_t>(Status::OK);
    resp_header.flags = 0;
    resp_header.reserved = 0;
    resp_header.value_len = sizeof(count);
    resp_header.request_id = header.request_id;

    response.resize(ResponseHeader::SIZE);
    std::memcpy(response.data(), &resp_header, ResponseHeader::SIZE);

    // Append count
    const uint8_t* count_bytes = reinterpret_cast<const uint8_t*>(&count);
    response.insert(response.end(), count_bytes, count_bytes + sizeof(count));

    return response;
}

std::vector<uint8_t> ProtocolHandler::handle_batch_get(
    const RequestHeader& header,
    const std::string& key,
    void* value) {

    // Batch format: key1\0key2\0key3\0...
    if (!storage_ || !value || header.value_len == 0) {
        return ProtocolCodec::encode_response(Status::ERROR, header.request_id);
    }

    const char* data = static_cast<const char*>(value);
    size_t offset = 0;
    size_t total = header.value_len;

    // First pass: count keys
    std::vector<std::string> keys;
    while (offset < total) {
        size_t key_len = strlen(data + offset);
        if (key_len == 0) break;

        keys.emplace_back(data + offset, key_len);
        offset += key_len + 1;
    }

    // Second pass: get values
    std::string response_data;
    int found_count = 0;

    for (const auto& k : keys) {
        std::vector<uint8_t> buffer(4096);
        size_t size = buffer.size();

        Status status = storage_->get(k, buffer.data(), &size);
        if (status == Status::OK) {
            // key\0value\0
            response_data += k;
            response_data += '\0';
            response_data.append(reinterpret_cast<char*>(buffer.data()), size);
            response_data += '\0';
            found_count++;
        }
    }

    // Return error if nothing found
    if (found_count == 0) {
        return ProtocolCodec::encode_response(Status::NOT_FOUND, header.request_id);
    }

    // Build response
    std::vector<uint8_t> response;
    ResponseHeader resp_header;
    resp_header.status = static_cast<uint8_t>(Status::OK);
    resp_header.flags = 0;
    resp_header.reserved = 0;
    resp_header.value_len = static_cast<uint32_t>(response_data.size());
    resp_header.request_id = header.request_id;

    response.resize(ResponseHeader::SIZE);
    std::memcpy(response.data(), &resp_header, ResponseHeader::SIZE);

    const uint8_t* data_bytes = reinterpret_cast<const uint8_t*>(response_data.data());
    response.insert(response.end(), data_bytes, data_bytes + response_data.size());

    return response;
}

std::vector<uint8_t> ProtocolHandler::handle_put_user_mem(
    const RequestHeader& header,
    const std::string& key,
    void* value) {

    if (!storage_) {
        return ProtocolCodec::encode_response(Status::ERROR, header.request_id);
    }

    // value contains: remote_addr (8 bytes) + rkey (4 bytes) + size (4 bytes)
    if (header.value_len < 16) {
        return ProtocolCodec::encode_response(Status::INVALID_MEMORY, header.request_id);
    }

    const uint8_t* data = static_cast<const uint8_t*>(value);
    uint64_t remote_addr = *reinterpret_cast<const uint64_t*>(data);
    uint32_t rkey = *reinterpret_cast<const uint32_t*>(data + 8);
    uint32_t size = *reinterpret_cast<const uint32_t*>(data + 12);

    Status status = storage_->put_user_mem(key,
        reinterpret_cast<void*>(remote_addr), rkey, size);

    return ProtocolCodec::encode_response(status, header.request_id);
}

std::vector<uint8_t> ProtocolHandler::handle_get_user_mem(
    const RequestHeader& header,
    const std::string& key,
    void* value) {

    if (!storage_) {
        return ProtocolCodec::encode_response(Status::ERROR, header.request_id);
    }

    // value contains: remote_addr (8 bytes) + rkey (4 bytes) + size (4 bytes)
    if (header.value_len < 16) {
        return ProtocolCodec::encode_response(Status::INVALID_MEMORY, header.request_id);
    }

    const uint8_t* data = static_cast<const uint8_t*>(value);
    uint64_t remote_addr = *reinterpret_cast<const uint64_t*>(data);
    uint32_t rkey = *reinterpret_cast<const uint32_t*>(data + 8);
    uint32_t size = *reinterpret_cast<const uint32_t*>(data + 12);

    Status status = storage_->get_user_mem(key,
        reinterpret_cast<void*>(remote_addr), rkey, size);

    return ProtocolCodec::encode_response(status, header.request_id);
}

} // namespace zerokv
