#pragma once

#include "zerokv/common.h"
#include "zerokv/config.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zerokv::kv {

struct KeyInfo {
    std::string key;
    size_t size = 0;
    uint64_t version = 0;
};

struct FetchResult {
    std::vector<std::byte> data;
    std::string owner_node_id;
    uint64_t version = 0;
};

struct FetchToItem {
    std::string key;
    size_t length = 0;
    size_t offset = 0;
};

struct FetchToManyResult {
    std::vector<std::string> completed;
    std::vector<std::string> failed;
    bool all_succeeded = false;
};

struct WaitKeysResult {
    std::vector<std::string> ready;
    std::vector<std::string> timed_out;
    bool completed = false;
};

struct BatchFetchResult {
    std::vector<std::pair<std::string, FetchResult>> fetched;
    std::vector<std::string> failed;
    std::vector<std::string> timed_out;
    bool completed = false;
};

struct BatchFetchToResult {
    std::vector<std::string> completed;
    std::vector<std::string> failed;
    std::vector<std::string> timed_out;
    bool completed_all = false;
};

struct PublishMetrics {
    uint64_t total_us = 0;
    uint64_t prepare_region_us = 0;
    uint64_t pack_rkey_us = 0;
    uint64_t put_meta_rpc_us = 0;
    bool ok = false;
};

struct FetchMetrics {
    uint64_t total_us = 0;
    uint64_t local_buffer_prepare_us = 0;
    uint64_t get_meta_rpc_us = 0;
    uint64_t peer_connect_us = 0;
    uint64_t rkey_prepare_us = 0;
    uint64_t get_submit_us = 0;
    uint64_t rdma_prepare_us = 0;
    uint64_t rdma_get_us = 0;
    uint64_t result_copy_us = 0;
    bool ok = false;
};

struct PushMetrics {
    uint64_t total_us = 0;
    uint64_t get_target_rpc_us = 0;
    uint64_t prepare_frame_us = 0;
    uint64_t rdma_put_flush_us = 0;
    uint64_t commit_rpc_us = 0;
    bool ok = false;
};

enum class SubscriptionEventType {
    kPublished,
    kUpdated,
    kUnpublished,
    kOwnerLost,
};

struct SubscriptionEvent {
    SubscriptionEventType type = SubscriptionEventType::kPublished;
    std::string key;
    std::string owner_node_id;
    uint64_t version = 0;
};

struct ServerConfig {
    std::string listen_addr;
};

struct NodeConfig {
    std::string server_addr;
    std::string local_data_addr;
    std::string node_id;
};

}  // namespace zerokv::kv

namespace zerokv::core {

using KeyInfo = ::zerokv::kv::KeyInfo;
using FetchResult = ::zerokv::kv::FetchResult;
using FetchToItem = ::zerokv::kv::FetchToItem;
using FetchToManyResult = ::zerokv::kv::FetchToManyResult;
using WaitKeysResult = ::zerokv::kv::WaitKeysResult;
using BatchFetchResult = ::zerokv::kv::BatchFetchResult;
using BatchFetchToResult = ::zerokv::kv::BatchFetchToResult;
using PublishMetrics = ::zerokv::kv::PublishMetrics;
using FetchMetrics = ::zerokv::kv::FetchMetrics;
using PushMetrics = ::zerokv::kv::PushMetrics;
using SubscriptionEventType = ::zerokv::kv::SubscriptionEventType;
using SubscriptionEvent = ::zerokv::kv::SubscriptionEvent;
using ServerConfig = ::zerokv::kv::ServerConfig;
using NodeConfig = ::zerokv::kv::NodeConfig;

}  // namespace zerokv::core
