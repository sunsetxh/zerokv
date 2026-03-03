// Batch operations support
#pragma once

#include <vector>
#include <string>
#include <utility>

namespace zerokv {

// Key-value pair
using KV = std::pair<std::string, std::string>;

// Batch results
struct BatchResult {
    int success_count;
    int failure_count;
    std::vector<bool> results;  // true = success
};

// Batch operations interface
class BatchOperations {
public:
    virtual ~BatchOperations() = default;

    // Batch put - put multiple key-value pairs
    virtual BatchResult batch_put(const std::vector<KV>& items) = 0;

    // Batch get - get multiple values by keys
    virtual std::vector<std::string> batch_get(const std::vector<std::string>& keys) = 0;

    // Batch delete - delete multiple keys
    virtual BatchResult batch_delete(const std::vector<std::string>& keys) = 0;

    // Batch exists - check if keys exist
    virtual std::vector<bool> batch_exists(const std::vector<std::string>& keys) = 0;
};

// Pipeline operations (batching without waiting)
class Pipeline {
public:
    Pipeline();

    // Add operation to pipeline (non-blocking)
    void put(const std::string& key, const std::string& value);
    void get(const std::string& key);
    void delete_key(const std::string& key);

    // Execute all operations in pipeline
    BatchResult execute();

    // Clear pipeline
    void clear();

    size_t size() const { return operations_.size(); }

private:
    enum class OpType { PUT, GET, DELETE };

    struct Operation {
        OpType type;
        std::string key;
        std::string value;
    };

    std::vector<Operation> operations_;
};

// Stream operations for large data
class StreamWriter {
public:
    StreamWriter(const std::string& key, size_t chunk_size = 64 * 1024);
    ~StreamWriter();

    // Write chunk
    bool write_chunk(const void* data, size_t size);

    // Finalize and close stream
    bool finalize();

private:
    std::string key_;
    size_t chunk_size_;
    int chunk_index_;
};

class StreamReader {
public:
    StreamReader(const std::string& key);
    ~StreamReader();

    // Read next chunk
    bool read_chunk(void* buffer, size_t* size);

    // Check if more data available
    bool has_more() const;

private:
    std::string key_;
    int chunk_index_;
};

} // namespace zerokv
