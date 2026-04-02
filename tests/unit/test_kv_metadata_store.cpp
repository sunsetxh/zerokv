#include "kv/metadata_store.h"

#include <gtest/gtest.h>

namespace {

using zerokv::core::detail::KeyMetadata;
using zerokv::core::detail::MetadataStore;
using zerokv::core::detail::NodeInfo;

NodeInfo make_node(std::string id, std::string data_addr = "10.0.0.1:14000") {
    NodeInfo node;
    node.node_id = std::move(id);
    node.control_addr = "10.0.0.1:13000";
    node.data_addr = std::move(data_addr);
    node.last_heartbeat_ms = 100;
    node.state = NodeInfo::State::kAlive;
    return node;
}

KeyMetadata make_meta(std::string key, std::string owner) {
    KeyMetadata meta;
    meta.key = std::move(key);
    meta.owner_node_id = std::move(owner);
    meta.owner_data_addr = "10.0.0.1:14000";
    meta.remote_addr = 0x1234;
    meta.rkey = {1, 2, 3};
    meta.size = 4096;
    meta.version = 7;
    return meta;
}

TEST(KvMetadataStoreTest, RegisterNodeAndHeartbeat) {
    MetadataStore store;
    EXPECT_TRUE(store.register_node(make_node("node-a")));

    auto node = store.get_node("node-a");
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->node_id, "node-a");
    EXPECT_EQ(node->state, NodeInfo::State::kAlive);

    EXPECT_TRUE(store.update_heartbeat("node-a", 999));
    node = store.get_node("node-a");
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->last_heartbeat_ms, 999u);
}

TEST(KvMetadataStoreTest, RejectsEmptyNodeIdAndHandlesMissingNodeUpdates) {
    MetadataStore store;
    EXPECT_FALSE(store.register_node(make_node("")));
    EXPECT_FALSE(store.update_heartbeat("missing", 10));
    EXPECT_FALSE(store.mark_node_dead("missing"));
}

TEST(KvMetadataStoreTest, RegisterNodeUpsertReplacesAddresses) {
    MetadataStore store;
    auto first = make_node("node-a", "10.0.0.1:14000");
    first.control_addr = "10.0.0.1:13000";
    EXPECT_TRUE(store.register_node(first));

    auto second = make_node("node-a", "10.0.0.2:14000");
    second.control_addr = "10.0.0.2:13000";
    EXPECT_TRUE(store.register_node(second));

    auto node = store.get_node("node-a");
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->control_addr, "10.0.0.2:13000");
    EXPECT_EQ(node->data_addr, "10.0.0.2:14000");
}

TEST(KvMetadataStoreTest, PutRequiresKnownLiveOwner) {
    MetadataStore store;
    EXPECT_FALSE(store.put(make_meta("k1", "node-a")));
    EXPECT_FALSE(store.put(make_meta("", "node-a")));

    EXPECT_TRUE(store.register_node(make_node("node-a")));
    EXPECT_TRUE(store.put(make_meta("k1", "node-a")));

    auto found = store.get("k1");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->owner_node_id, "node-a");
    EXPECT_EQ(found->size, 4096u);
}

TEST(KvMetadataStoreTest, AllowsOwnerOverwriteButRejectsCrossOwnerOverwrite) {
    MetadataStore store;
    EXPECT_TRUE(store.register_node(make_node("node-a")));
    EXPECT_TRUE(store.register_node(make_node("node-b", "10.0.0.2:14000")));

    EXPECT_TRUE(store.put(make_meta("k1", "node-a")));
    auto owner_update = make_meta("k1", "node-a");
    owner_update.version = 8;
    EXPECT_TRUE(store.put(owner_update));
    EXPECT_FALSE(store.put(make_meta("k1", "node-b")));

    auto found = store.get("k1");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->owner_node_id, "node-a");
    EXPECT_EQ(found->version, 8u);
    EXPECT_EQ(found->owner_data_addr, "10.0.0.1:14000");
}

TEST(KvMetadataStoreTest, EraseRequiresMatchingOwner) {
    MetadataStore store;
    EXPECT_TRUE(store.register_node(make_node("node-a")));
    EXPECT_TRUE(store.put(make_meta("k1", "node-a")));

    EXPECT_FALSE(store.erase("k1", "node-b"));
    EXPECT_TRUE(store.erase("k1", "node-a"));
    EXPECT_FALSE(store.get("k1").has_value());
}

TEST(KvMetadataStoreTest, MarkNodeDeadRemovesOwnedKeys) {
    MetadataStore store;
    EXPECT_TRUE(store.register_node(make_node("node-a")));
    EXPECT_TRUE(store.register_node(make_node("node-b", "10.0.0.2:14000")));
    EXPECT_TRUE(store.put(make_meta("a", "node-a")));
    EXPECT_TRUE(store.put(make_meta("b", "node-b")));
    EXPECT_TRUE(store.get("a").has_value());
    EXPECT_TRUE(store.get_active("a").has_value());

    EXPECT_TRUE(store.mark_node_dead("node-a"));
    EXPECT_FALSE(store.get("a").has_value());
    EXPECT_FALSE(store.get_active("a").has_value());
    EXPECT_TRUE(store.get("b").has_value());

    auto node = store.get_node("node-a");
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->state, NodeInfo::State::kDead);
}

TEST(KvMetadataStoreTest, ListKeysAndListByOwnerAreStable) {
    MetadataStore store;
    EXPECT_TRUE(store.register_node(make_node("node-a")));
    EXPECT_TRUE(store.register_node(make_node("node-b", "10.0.0.2:14000")));
    EXPECT_TRUE(store.put(make_meta("z-key", "node-a")));
    EXPECT_TRUE(store.put(make_meta("a-key", "node-a")));
    EXPECT_TRUE(store.put(make_meta("m-key", "node-b")));

    auto keys = store.list_keys();
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys[0], "a-key");
    EXPECT_EQ(keys[1], "m-key");
    EXPECT_EQ(keys[2], "z-key");

    auto owned = store.list_by_owner("node-a");
    ASSERT_EQ(owned.size(), 2u);
    EXPECT_EQ(owned[0].key, "a-key");
    EXPECT_EQ(owned[1].key, "z-key");

    auto missing = store.list_by_owner("missing");
    EXPECT_TRUE(missing.empty());
}

TEST(KvMetadataStoreTest, GetMissingKeyReturnsNullopt) {
    MetadataStore store;
    EXPECT_FALSE(store.get("missing").has_value());
    EXPECT_FALSE(store.get_active("missing").has_value());
}

TEST(KvMetadataStoreTest, ReregisterDeadNodeRevivesButDoesNotRestoreKeys) {
    MetadataStore store;
    EXPECT_TRUE(store.register_node(make_node("node-a")));
    EXPECT_TRUE(store.put(make_meta("k1", "node-a")));
    EXPECT_TRUE(store.mark_node_dead("node-a"));

    EXPECT_FALSE(store.get("k1").has_value());
    auto node = store.get_node("node-a");
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->state, NodeInfo::State::kDead);

    EXPECT_TRUE(store.register_node(make_node("node-a")));
    node = store.get_node("node-a");
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->state, NodeInfo::State::kAlive);

    EXPECT_FALSE(store.get("k1").has_value());
    EXPECT_TRUE(store.put(make_meta("k2", "node-a")));
    EXPECT_TRUE(store.get("k2").has_value());
}

TEST(KvMetadataStoreTest, RegisterNodeStoresPushMetadata) {
    MetadataStore store;

    NodeInfo node;
    node.node_id = "node-push";
    node.control_addr = "127.0.0.1:19001";
    node.data_addr = "127.0.0.1:20001";
    node.push_control_addr = "127.0.0.1:21001";
    node.push_inbox_remote_addr = 0xABCDEF;
    node.push_inbox_rkey = {1, 2, 3, 4};
    node.push_inbox_capacity = 4096;

    EXPECT_TRUE(store.register_node(node));

    auto found = store.get_node("node-push");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->push_control_addr, "127.0.0.1:21001");
    EXPECT_EQ(found->push_inbox_remote_addr, 0xABCDEFu);
    EXPECT_EQ(found->push_inbox_rkey, (std::vector<uint8_t>{1, 2, 3, 4}));
    EXPECT_EQ(found->push_inbox_capacity, 4096u);
}

}  // namespace
