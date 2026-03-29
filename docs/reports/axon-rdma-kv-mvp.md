# AXON RDMA KV MVP

## 1. Product Goal

The MVP goal is not to build a full distributed KV system. It is to build a
working RDMA-based async communication prototype with:

- 1 server
- N clients
- server-owned metadata directory
- client-to-client data transfer through RDMA
- server optionally acting as a client

The target closed loop is:

1. server starts
2. client starts and connects to server
3. client1 publishes a `key -> value`
4. server stores metadata for the key
5. client2 queries metadata by key from server
6. client2 fetches value directly from client1 via RDMA
7. server can also publish/fetch like a normal client

## 2. Scope

### In Scope

- single server
- multiple clients
- node registration
- metadata registration
- metadata lookup
- client-to-client RDMA get
- server-as-client
- basic failure reporting

### Out of Scope

- multi-server
- replication
- consensus
- persistence
- ACL / authorization
- atomic update semantics
- automatic failover
- complex recovery

## 3. MVP Success Criteria

Functional:

- server plus 2 clients can start and connect
- client1 can publish a key
- server can return metadata for that key
- client2 can fetch the value from client1 via RDMA
- server can also publish/fetch using the same model

Validation:

- repeated publish/fetch loop succeeds without crash
- owner disconnect makes metadata unavailable or stale
- 4 KiB, 64 KiB, 1 MiB fetch path works

## 4. Architecture

The MVP uses two planes:

### Control Plane

`client <-> server`

Used for:

- node registration
- key metadata registration
- metadata lookup
- unpublish
- heartbeat

### Data Plane

`client <-> client`

Used for:

- RDMA get
- optional future RDMA put

Server holds metadata only. Large value payload is not forwarded by server.

## 5. Mapping to Current AXON

Current AXON already provides the transport core:

- `Context`
- `Worker`
- `Listener`
- `Endpoint`
- `MemoryRegion`
- `RemoteKey`
- `Future`

The following are already usable for MVP:

- `listen/connect`
- async progress thread
- tag messaging for control-plane requests
- memory registration
- packed remote key exchange
- RDMA `get`

The following are not MVP blockers and can be deferred:

- atomic operations
- advanced future chaining
- full memory pool / registration cache support

## 6. New MVP Components

### KVServer

Responsibilities:

- accept client registrations
- maintain global metadata directory
- respond to key lookup requests
- track node liveness

### KVNode

Responsibilities:

- connect to server
- register itself
- publish local keys
- fetch remote keys
- maintain peer endpoint cache

### MetadataStore

Responsibilities:

- hold `key -> metadata`
- update on publish/unpublish
- invalidate on node loss

### ControlProtocol

Responsibilities:

- define message types
- serialize / deserialize control-plane messages

### PeerManager

Responsibilities:

- cache peer endpoints
- connect to peer by advertised data-plane address
- reuse connection for repeated fetch

## 7. Core Data Structures

### NodeInfo

- `node_id`
- `control_addr`
- `data_addr`
- `last_heartbeat`
- `state`

### KeyMetadata

- `key`
- `owner_node_id`
- `owner_data_addr`
- `remote_addr`
- `rkey`
- `size`
- `version`
- `state`

### PublishedObject

- `key`
- `MemoryRegion::Ptr`
- `remote_addr`
- `RemoteKey`
- `size`
- `version`

## 8. MVP APIs

### KVServer

- `start(ServerConfig)`
- `stop()`
- `lookup(key)`
- `list_keys()`
- `is_running()`
- `address()`

### KVNode

- `start(NodeConfig)`
- `stop()`
- `publish(key, buffer, size)`
- `fetch(key)`
- `fetch_to(key, local_region, length, local_offset=0)`
- `unpublish(key)`
- `is_running()`
- `node_id()`
- `published_count()`

Public API note:

- user-facing lookup should only expose `key`, `size`, and `version`
- RDMA routing details such as owner address, remote address, and rkey stay internal
- `publish(key, data, size)` should copy data into implementation-managed memory
- `publish_region(key, region, size)` is the zero-copy path and requires the caller to keep the region alive

## 9. Control Protocol

Recommended message types:

- `REGISTER_NODE`
- `REGISTER_NODE_RESP`
- `PUT_META`
- `PUT_META_RESP`
- `GET_META`
- `GET_META_RESP`
- `UNPUBLISH`
- `HEARTBEAT`

Recommended common header:

- `type`
- `request_id`
- `payload_length`

### PUT_META payload

- `key`
- `owner_node_id`
- `data_addr`
- `remote_addr`
- `rkey_blob`
- `size`
- `version`

### GET_META payload

- `key`

### GET_META_RESP payload

- `status`
- `owner_node_id`
- `data_addr`
- `remote_addr`
- `rkey_blob`
- `size`
- `version`

## 10. Main Flows

### Node Start

1. create local listener for peer data connections
2. connect to server
3. send `REGISTER_NODE`
4. enter steady state

### Publish

1. caller provides `key` and local buffer
2. register or allocate `MemoryRegion`
3. pack `RemoteKey`
4. send `PUT_META` to server
5. server updates metadata table

### Fetch

1. caller requests `fetch(key)`
2. node sends `GET_META` to server
3. server returns `KeyMetadata`
4. node connects to owner if needed
5. node allocates local region
6. node performs RDMA `get`
7. node returns fetched bytes

### Server As Client

Server process hosts both:

- `KVServer`
- `KVNode`

This keeps the model uniform and avoids special-case data path logic.

## 11. Risks

### Product Risks

- stale metadata after owner crash
- unclear overwrite policy for duplicate keys
- memory lifetime errors causing invalid remote access

### Technical Risks

- endpoint cache can become stale
- current AXON sockaddr path is IPv4-literal oriented
- current error callback path is weak
- current atomic path is not production-ready

## 12. Build Order

Recommended implementation order:

1. `ControlProtocol`
2. `MetadataStore`
3. `KVServer`
4. `KVNode::start()` and node registration
5. `publish()`
6. `lookup()` and `fetch()`
7. server-as-client
8. heartbeat / stale cleanup
9. benchmark and fault-path validation

## 13. TODO Extensions

The following items are explicitly recorded for follow-up after the first MVP
closure:

1. support direct `put` to a specified client

- allow a caller to target a known destination node directly
- bypass server lookup when the destination is already known
- useful for push-style workflows in addition to current fetch/get flow

2. support end-to-end performance instrumentation

- add full-path timing for control-plane and data-plane stages
- measure publish, lookup, connect, rkey handling, RDMA transfer, and total fetch latency
- expose metrics suitable for both debugging and benchmark reports

3. support subscription

- allow clients to subscribe to key lifecycle changes such as publish, update,
  unpublish, and owner loss
- server should act as the metadata event source and fan out notifications to
  interested subscribers
- define delivery semantics explicitly in a later phase:
  best-effort vs acknowledged delivery, replay behavior for late subscribers,
  and ordering guarantees relative to publish/unpublish

## 14. Practical Conclusion

Current AXON is already strong enough to support the MVP transport layer.
What is still missing is not the core RDMA mechanism, but the control-plane
and product-level object model:

- node registration
- metadata directory
- publish / lookup / fetch APIs
- peer routing and lifecycle handling

The right next step is to build the MVP control plane on top of AXON rather
than continuing to broaden low-level transport APIs first.

## 15. Known Limitations

- UCX 1.20 + Soft-RoCE (`rxe0`) + cross-VM KV fetch may hit a UCX internal
  assertion in `proto_select.c` when using the new protocol stack.
  This was observed with the KV demo across the two QEMU VMs.

- This is not a general AXON RDMA failure:
  - `rdma_put_get` still works cross-VM in the same environment.
  - KV fetch works inside a single VM.
  - Cross-VM KV fetch succeeds when the UCX new protocol stack is disabled.

- Environment-specific workaround for the current QEMU Soft-RoCE setup:

```bash
export UCX_PROTO_ENABLE=n
export UCX_NET_DEVICES=rxe0:1
```

- Recommended demo usage for this environment:

```bash
# VM1: server
./kv_demo --mode server --listen 10.0.0.1:15000 --transport rdma

# VM1: publisher
UCX_PROTO_ENABLE=n UCX_NET_DEVICES=rxe0:1 \
./kv_demo --mode publish --server-addr 10.0.0.1:15000 \
  --data-addr 10.0.0.1:0 --node-id publisher --key mykey \
  --value hello-rdma-world --transport rdma --hold

# VM2: reader
UCX_PROTO_ENABLE=n UCX_NET_DEVICES=rxe0:1 \
./kv_demo --mode fetch --server-addr 10.0.0.1:15000 \
  --data-addr 10.0.0.2:0 --node-id reader --key mykey \
  --transport rdma
```

- Atomic operations remain out of scope for this environment because Soft-RoCE
  does not provide working hardware atomics here.
