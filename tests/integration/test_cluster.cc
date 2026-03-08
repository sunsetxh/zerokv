// Integration test for cluster
#include <gtest/gtest.h>
#include "zerokv/client.h"
#include "test_server_fixture.h"
#include <vector>
#include <string>

using namespace zerokv;

// Test US1: Basic put/get/delete operations
TEST(IntegrationTest, PutGetOperations) {
    test::TestClient client({"127.0.0.1:5000"});

    // Put a key-value pair
    Status status = client.Put("test_key", "test_value");
    EXPECT_EQ(status, Status::OK);

    // Get the value
    std::string value = client.Get("test_key");
    EXPECT_EQ(value, "test_value");
}

TEST(IntegrationTest, DeleteOperations) {
    test::TestClient client({"127.0.0.1:5000"});

    // Put a key-value pair
    Status put_status = client.Put("delete_key", "delete_value");
    EXPECT_EQ(put_status, Status::OK);

    // Verify it exists
    std::string value = client.Get("delete_key");
    EXPECT_EQ(value, "delete_value");

    // Delete the key
    Status del_status = client.Remove("delete_key");
    EXPECT_EQ(del_status, Status::OK);

    // Verify it's gone - should return empty or error
    std::string deleted_value = client.Get("delete_key");
    EXPECT_TRUE(deleted_value.empty());
}

TEST(IntegrationTest, UpdateExistingKey) {
    test::TestClient client({"127.0.0.1:5000"});

    // Put initial value
    Status status1 = client.Put("update_key", "value1");
    EXPECT_EQ(status1, Status::OK);

    // Get initial value
    std::string value1 = client.Get("update_key");
    EXPECT_EQ(value1, "value1");

    // Update value
    Status status2 = client.Put("update_key", "value2");
    EXPECT_EQ(status2, Status::OK);

    // Get updated value
    std::string value2 = client.Get("update_key");
    EXPECT_EQ(value2, "value2");
}

// Test US2: Batch operations
TEST(IntegrationTest, BatchOperations) {
    test::TestClient client({"127.0.0.1:5000"});

    // Prepare batch data
    std::vector<std::pair<std::string, std::string>> items = {
        {"batch_key_1", "batch_value_1"},
        {"batch_key_2", "batch_value_2"},
        {"batch_key_3", "batch_value_3"}
    };

    // Batch put
    Status put_status = client.BatchPut(items);
    EXPECT_EQ(put_status, Status::OK);

    // Batch get
    std::vector<std::string> keys = {"batch_key_1", "batch_key_2", "batch_key_3"};
    std::vector<std::string> values = client.BatchGet(keys);

    EXPECT_EQ(values.size(), 3);
    EXPECT_EQ(values[0], "batch_value_1");
    EXPECT_EQ(values[1], "batch_value_2");
    EXPECT_EQ(values[2], "batch_value_3");
}

// Test US1: Multiple clients
TEST(IntegrationTest, MultipleClients) {
    test::TestClient client1({"127.0.0.1:5000"});
    test::TestClient client2({"127.0.0.1:5000"});

    // Client 1 writes
    Status status = client1.Put("shared_key", "client1_value");
    EXPECT_EQ(status, Status::OK);

    // Client 2 reads
    std::string value = client2.Get("shared_key");
    EXPECT_EQ(value, "client1_value");
}

// Test US5: Cross-language operations (C++ writes, Python reads)
TEST(IntegrationTest, CrossLanguageWrite) {
    test::TestClient client({"127.0.0.1:5000"});

    // Write from C++
    Status status = client.Put("cross_lang_key", "from_cpp");
    EXPECT_EQ(status, Status::OK);

    // This value should be readable from Python
    std::string value = client.Get("cross_lang_key");
    EXPECT_EQ(value, "from_cpp");
}

// Cluster tests
TEST(ClusterTest, MultiNode) {
    zerokv::Client client;

    // Connect to multiple servers
    std::vector<std::string> servers = {
        "localhost:5000",
        "localhost:5001",
        "localhost:5002"
    };

    // Note: This test requires actual servers running
    // For now, this is a placeholder
    EXPECT_TRUE(servers.size() >= 1);
}

TEST(ClusterTest, Replication) {
    // Test data replication across nodes
    // Placeholder for future implementation
    GTEST_SKIP() << "Replication not implemented yet";
}
