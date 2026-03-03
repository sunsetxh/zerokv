// Test batch operations
#include <iostream>
#include <vector>
#include <string>
#include "zerokv/batch.h"
#include "zerokv/storage.h"

using namespace zerokv;

static bool ok(Status s) { return s == Status::OK; }

void test_batch_put() {
    std::cout << "\n=== Test: Batch Put ===" << std::endl;

    auto storage = std::make_unique<StorageEngine>(10 * 1024 * 1024);

    std::vector<KV> items;
    for (int i = 0; i < 100; i++) {
        std::string key = "batch_" + std::to_string(i);
        std::string value = "value_" + std::to_string(i);
        items.emplace_back(key, value);
    }

    // Simulate batch put
    int success = 0;
    for (const auto& [key, value] : items) {
        if (ok(storage->put(key, value.data(), value.size()))) {
            success++;
        }
    }

    std::cout << "[Batch] Inserted " << success << "/" << items.size() << std::endl;
}

void test_pipeline() {
    std::cout << "\n=== Test: Pipeline ===" << std::endl;

    Pipeline pipeline;

    // Add operations
    for (int i = 0; i < 10; i++) {
        pipeline.put("key_" + std::to_string(i), "value_" + std::to_string(i));
    }

    std::cout << "[Pipeline] Added " << pipeline.size() << " operations" << std::endl;

    auto result = pipeline.execute();
    std::cout << "[Pipeline] Executed, success=" << result.success_count << std::endl;

    pipeline.clear();
    std::cout << "[Pipeline] Cleared, size=" << pipeline.size() << std::endl;
}

void test_stream() {
    std::cout << "\n=== Test: Stream ===" << std::endl;

    StreamWriter writer("large_data", 1024);

    // Write chunks
    for (int i = 0; i < 5; i++) {
        char data[1024];
        memset(data, 'A' + i, 1024);
        writer.write_chunk(data, 1024);
    }

    writer.finalize();
    std::cout << "[Stream] Wrote 5 chunks" << std::endl;
}

int main() {
    std::cout << "╔═══════════════════════════════════════╗" << std::endl;
    std::cout << "║  ZeroKV Batch Operations Test       ║" << std::endl;
    std::cout << "╚═══════════════════════════════════════╝" << std::endl;

    test_batch_put();
    test_pipeline();
    test_stream();

    std::cout << "\n=== All batch tests completed ===" << std::endl;
    return 0;
}
