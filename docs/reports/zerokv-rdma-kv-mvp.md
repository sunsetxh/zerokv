# ZeroKV RDMA KV MVP

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

## 5. Mapping to Current ZeroKV

Current ZeroKV already provides the transport core:

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

## 6. Role Model

The system has only two real node roles:

- `Server`
  - control-plane role
  - stores node registrations and key metadata
  - serves lookup requests
  - fans out subscription events
  - does not forward large value payloads
- `KVNode`
  - data-plane role
  - owns registered memory
  - performs RDMA reads and writes
  - can publish, fetch, and push data

`publish`, `fetch`, and `push` are not different node types. They are
different operation modes of the same `KVNode`.

### Operation Roles

#### Publish

- node stores `key -> value` in its own local registered memory
- node becomes the `owner`
- server stores metadata only
- later readers fetch from the owner node

#### Fetch

- node asks server for key metadata
- node acts as the `reader`
- node connects to the `owner`
- node reads the value directly from the owner through RDMA

#### Push

- node acts as the `sender`
- sender writes `key + value` into a specific target node
- target becomes the new `owner`
- later readers fetch from the target, not from the original sender

### Owner Semantics

- after `publish`, `owner = publisher`
- during `fetch`, owner does not change
- after `push`, `owner = target`

The same `KVNode` may perform all three operations in one process lifetime:

- publish one key
- fetch another key
- push a third key to a different node

Recommended terminology:

- node roles:
  - `Server`
  - `KVNode`
- operation modes:
  - `publish`
  - `fetch`
  - `push`
- data-flow identities:
  - `owner`
  - `reader`
  - `sender`
  - `target`

## 7. New MVP Components

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

## 8. Core Data Structures

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

## 9. MVP APIs

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
- `publish_region(key, region, size)`
- `fetch(key)` -> `Future<FetchResult>`
- `fetch_to(key, local_region, length, local_offset=0)`
- `push(target_node_id, key, data, size)`
- `unpublish(key)`
- `subscribe(key)` / `unsubscribe(key)`
- `drain_subscription_events()` -> `vector<SubscriptionEvent>`
- `wait_for_key(key, timeout)` -> `Status`
- `wait_for_keys(keys, timeout)` -> `WaitKeysResult`
- `subscribe_and_fetch_once(key, timeout)` -> `FetchResult`
- `subscribe_and_fetch_once_many(keys, timeout)` -> `BatchFetchResult`
- `is_running()`
- `node_id()`
- `published_count()`
- `last_publish_metrics()` / `last_fetch_metrics()` / `last_push_metrics()`

Public API note:

- user-facing lookup should only expose `key`, `size`, and `version`
- RDMA routing details such as owner address, remote address, and rkey stay internal
- `publish(key, data, size)` should copy data into implementation-managed memory
- `publish_region(key, region, size)` is the zero-copy path and requires the caller to keep the region alive
- `fetch(key)` is the convenience API and includes a final copy into `FetchResult::data`
- `fetch_to(key, local_region, length, local_offset)` is the primary zero-copy public fetch API

### MessageKV Phase 1

- `MessageKV` Phase 1 wraps `publish` / `publish_region` and
  `subscribe_and_fetch_to_once_many`.
- Internal ack markers provide bounded, best-effort sender-side cleanup without
  a background thread.

## 10. Control Protocol

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
- current ZeroKV sockaddr path is IPv4-literal oriented
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

## 13. Completed Extensions

The following extensions have been implemented since the initial MVP closure.

### 13.1 Direct put / push (completed)

- `KVNode::push(target_node_id, key, data, size)` — RDMA write to a target node
- Target node becomes the new owner
- Sender queries server for target's push inbox address, performs RDMA put,
  then sends a direct TCP PUSH_COMMIT to the target for finalization
- PushMetrics exposed: get_target_rpc_us, prepare_frame_us, rdma_put_flush_us,
  commit_rpc_us

### 13.2 Performance instrumentation (completed)

- `PublishMetrics`: total_us, prepare_region_us, pack_rkey_us, put_meta_rpc_us
- `FetchMetrics`: total_us, local_buffer_prepare_us, get_meta_rpc_us,
  peer_connect_us, rdma_prepare_us, rdma_get_us
- `PushMetrics`: total_us, get_target_rpc_us, prepare_frame_us,
  rdma_put_flush_us, commit_rpc_us
- Last-sample accessor on KVNode: `last_publish_metrics()`,
  `last_fetch_metrics()`, `last_push_metrics()`

### 13.3 Subscription (completed)

- `KVNode::subscribe(key)` / `KVNode::unsubscribe(key)` — synchronous RPCs
- `KVNode::drain_subscription_events()` — polling API, swap-and-return
- Server fan-out: TCP connect → send event → close (best-effort, short-lived)
- Event types: kPublished, kUpdated, kUnpublished, kOwnerLost
- Ref-counted local subscriptions to protect pre-existing manual subscriptions

### 13.4 Wait-and-fetch (completed)

- `KVNode::wait_for_key(key, timeout)` — synchronous, returns Status
- `KVNode::wait_for_keys(keys, timeout)` — batch wait, partial results
- `KVNode::subscribe_and_fetch_once(key, timeout)` — wait + fetch single key
- `KVNode::subscribe_and_fetch_once_many(keys, timeout)` — batch, per-key as ready
- First-success-wins semantics, 100ms lookup fallback, deduplication

### 13.5 Build system improvements (completed)

- `UCX_ROOT` CMake option for non-system UCX installations
- Vendored dependency support under `third_party/` (googletest, benchmark,
  nanobind, ucx)
- Source packaging script (`scripts/package_source.sh`) with automatic vendor
  download from release tarballs

## 14. Practical Conclusion

Current ZeroKV is already strong enough to support the MVP transport layer.
What is still missing is not the core RDMA mechanism, but the control-plane
and product-level object model:

- node registration
- metadata directory
- publish / lookup / fetch APIs
- peer routing and lifecycle handling

The right next step is to build the MVP control plane on top of ZeroKV rather
than continuing to broaden low-level transport APIs first.

## 15. Known Limitations

- UCX 1.20 + Soft-RoCE (`rxe0`) + cross-VM KV fetch may hit a UCX internal
  assertion in `proto_select.c` when using the new protocol stack.
  This was observed with the KV demo across the two QEMU VMs.

- This is not a general ZeroKV RDMA failure:
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

## 16. Real RDMA Validation

The current ZeroKV KV stack is ready for functional validation on real RDMA
hardware.

Recommended validation order:

1. `publish -> fetch`
2. `push -> fetch`
3. `unpublish`
4. `subscription`

Do not start with large benchmarks. First confirm correctness on real NICs,
then use the built-in metrics to inspect latency.

### Environment Checklist

- two hosts with working RDMA NICs and reachable IP connectivity
- `ibv_devinfo` succeeds on both hosts
- `ucx_info -d` shows the expected RDMA transport on both hosts
- `kv_demo` is built from the current tree
- `UCX_NET_DEVICES` is set to the target RDMA device when needed

Unlike the QEMU Soft-RoCE setup, real NIC validation should start without the
`UCX_PROTO_ENABLE=n` workaround. Only add environment workarounds if the real
environment demonstrates the same UCX issue.

### Functional Smoke Test

Start the server on the metadata host:

```bash
./kv_demo --mode server --listen <server_ip>:15000 --transport rdma
```

On node A, publish a key and keep the owner alive:

```bash
UCX_NET_DEVICES=<rdma_dev> ./kv_demo \
  --mode publish \
  --server-addr <server_ip>:15000 \
  --data-addr <node_a_ip>:0 \
  --node-id node-a \
  --key k1 \
  --value hello-rdma \
  --transport rdma \
  --hold
```

On node B, fetch the key:

```bash
UCX_NET_DEVICES=<rdma_dev> ./kv_demo \
  --mode fetch \
  --server-addr <server_ip>:15000 \
  --data-addr <node_b_ip>:0 \
  --node-id node-b \
  --key k1 \
  --transport rdma
```

Expected result:

- fetch succeeds
- fetched value matches `hello-rdma`
- output includes `fetch_metrics`

### Push Validation

Start a target node on node B by launching any node mode first. The simplest
way is to keep a published key alive:

```bash
UCX_NET_DEVICES=<rdma_dev> ./kv_demo \
  --mode publish \
  --server-addr <server_ip>:15000 \
  --data-addr <node_b_ip>:0 \
  --node-id node-b \
  --key existing \
  --value keepalive \
  --transport rdma \
  --hold
```

Then, on node A, push a new key to node B:

```bash
UCX_NET_DEVICES=<rdma_dev> ./kv_demo \
  --mode push \
  --server-addr <server_ip>:15000 \
  --data-addr <node_a_ip>:0 \
  --node-id node-a \
  --target-node-id node-b \
  --key pushed-key \
  --value pushed-data \
  --transport rdma
```

Finally, from a third node or from node A, fetch the pushed key:

```bash
UCX_NET_DEVICES=<rdma_dev> ./kv_demo \
  --mode fetch \
  --server-addr <server_ip>:15000 \
  --data-addr <node_c_ip>:0 \
  --node-id node-c \
  --key pushed-key \
  --transport rdma
```

Expected result:

- fetch succeeds
- fetched value matches `pushed-data`
- fetch output reports `owner=node-b`
- push output includes `push_metrics`

### Unpublish Validation

From the owner node, remove a key:

```bash
UCX_NET_DEVICES=<rdma_dev> ./kv_demo \
  --mode unpublish \
  --server-addr <server_ip>:15000 \
  --data-addr <owner_ip>:0 \
  --node-id node-a \
  --key k1 \
  --transport rdma
```

Then try to fetch it again from another node.

Expected result:

- unpublish succeeds
- subsequent fetch fails with a not-found style error

### Subscription Validation

Use the dedicated `kv_wait_fetch` example for the "subscribe before key exists,
then fetch when it appears" scenario.

Example cross-host flow:

```bash
# Host A: server
./kv_demo --mode server --listen 10.0.0.1:15150 --transport rdma

# Host A: waiter
UCX_PROTO_ENABLE=n UCX_NET_DEVICES=rxe0:1 ./kv_wait_fetch \
  --mode subscribe-fetch-once \
  --server-addr 10.0.0.1:15150 \
  --data-addr 10.0.0.1:0 \
  --node-id waiter \
  --key waitfetch-key \
  --transport rdma

# Host B: publisher
UCX_PROTO_ENABLE=n UCX_NET_DEVICES=rxe0:1 ./kv_demo \
  --mode publish \
  --server-addr 10.0.0.1:15150 \
  --data-addr 10.0.0.2:0 \
  --node-id publisher \
  --key waitfetch-key \
  --value hello-waitfetch \
  --transport rdma \
  --hold
```

Observed result from the two-VM Soft-RoCE setup:

```text
FETCH_OK key=waitfetch-key owner=publisher-waitfetch version=1 value=hello-waitfetch
```

This validates:

- subscribing to a non-existent key
- later publish on a different node
- event-driven fetch on the waiter
- cross-node RDMA read from the eventual owner

### Metrics to Inspect

The current implementation exposes three last-sample metrics snapshots:

- `PublishMetrics`
- `FetchMetrics`
- `PushMetrics`

For the first real RDMA pass, focus on:

- `publish total_us`
- `fetch total_us`
- `fetch rdma_get_us`
- `push total_us`
- `push rdma_put_flush_us`

These are enough to determine whether latency is dominated by control-plane
lookup, peer connection setup, or RDMA transfer itself.

### KV Benchmark

Use `kv_bench` for size-sweep publish and fetch benchmarking.

Start the benchmark server:

```bash
./kv_bench --mode server --listen <server_ip>:15000 --transport rdma
```

`bench-publish` does not need a separate owner process. The benchmark node
publishes data to its own local memory and is therefore the owner for those
temporary keys.

Only `bench-fetch` needs a separate stable owner process that keeps the
benchmark keys published:

```bash
UCX_NET_DEVICES=<rdma_dev> ./kv_bench \
  --mode hold-owner \
  --server-addr <server_ip>:15000 \
  --data-addr <owner_ip>:0 \
  --node-id owner \
  --transport rdma
```

Run publish benchmark:

```bash
UCX_NET_DEVICES=<rdma_dev> ./kv_bench \
  --mode bench-publish \
  --server-addr <server_ip>:15000 \
  --data-addr <client_ip>:0 \
  --node-id bench-publish \
  --publish-api copy \
  --sizes 4K,64K,1M,4M,16M,32M,64M,128M \
  --iters 4 \
  --transport rdma

UCX_NET_DEVICES=<rdma_dev> ./kv_bench \
  --mode bench-publish \
  --server-addr <server_ip>:15000 \
  --data-addr <client_ip>:0 \
  --node-id bench-publish-region \
  --publish-api region \
  --sizes 4K,64K,1M,4M,16M,32M,64M,128M \
  --iters 4 \
  --transport rdma
```

Run fetch benchmark:

```bash
UCX_NET_DEVICES=<rdma_dev> ./kv_bench \
  --mode bench-fetch \
  --server-addr <server_ip>:15000 \
  --data-addr <client_ip>:0 \
  --node-id bench-fetch \
  --sizes 4K,64K,1M,4M,16M,32M,64M,128M \
  --iters 4 \
  --transport rdma
```

Run zero-copy fetch benchmark:

```bash
UCX_NET_DEVICES=<rdma_dev> ./kv_bench \
  --mode bench-fetch-to \
  --server-addr <server_ip>:15000 \
  --data-addr <client_ip>:0 \
  --node-id bench-fetch-to \
  --sizes 4K,64K,1M,4M,16M,32M,64M,128M \
  --iters 4 \
  --transport rdma
```

Notes:

- `--iters N` is the recommended first-pass mode for cross-machine validation
- `--total-bytes SIZE` is still supported when you want each size to transfer a
  comparable total payload volume
- `hold-owner` publishes stable keys named `bench-fetch-<size-bytes>`
- `bench-publish` uses unique keys and unpublishes after each iteration to
  avoid metadata accumulation
- `bench-publish --publish-api copy` uses `publish()`
- `bench-publish --publish-api region` uses `publish_region()` and lets you
  compare copy-publish vs zero-copy publish on the same size sweep
- `--owner-node-id` is optional; if provided, the benchmark validates that the
  fetched owner matches the expected node id from metadata
- `bench-fetch` is the application-level E2E fetch benchmark and includes the
  final result copy
- `bench-fetch-to` is the zero-copy benchmark and is the better first choice
  when you want to reason about data-plane fetch cost
- benchmark throughput columns are reported in `MiB/s` (`throughput_MiBps`)

### Two-VM Soft-RoCE Benchmark Snapshot

The first end-to-end benchmark pass was run on the QEMU VM pair with:

- VM1: benchmark server
- VM1: fetch owner
- VM2: benchmark client
- `UCX_NET_DEVICES=rxe0:1`
- `UCX_PROTO_ENABLE=n`
- fixed `--iters 4`
- TCP control plane, RDMA data plane

This topology was chosen because it gives a real cross-VM RDMA fetch path while
avoiding an instability seen when `hold-owner` runs on VM2.

Observed publish results:

| Size | Iters | Avg Total | Avg Prepare | Avg Pack RKey | Avg PutMeta RPC | Throughput |
|---|---:|---:|---:|---:|---:|---:|
| 4KiB | 4 | 222.25 us | 4.00 us | 1.00 us | 196.75 us | 17.58 MB/s |
| 64KiB | 4 | 194.00 us | 37.50 us | 1.00 us | 151.25 us | 322.16 MB/s |
| 1MiB | 4 | 515.00 us | 362.75 us | 1.00 us | 147.75 us | 1941.75 MB/s |
| 4MiB | 4 | 1286.25 us | 1107.75 us | 1.00 us | 173.75 us | 3109.82 MB/s |
| 16MiB | 4 | 4601.75 us | 4359.25 us | 1.75 us | 234.75 us | 3476.94 MB/s |
| 32MiB | 4 | 7807.25 us | 7488.25 us | 4.75 us | 305.25 us | 4098.75 MB/s |
| 64MiB | 4 | 13801.75 us | 13441.75 us | 7.25 us | 338.25 us | 4637.09 MB/s |
| 128MiB | 4 | 24159.00 us | 23800.75 us | 8.75 us | 332.50 us | 5298.23 MB/s |

Observed fetch results that were stable in this environment:

| Size | Iters | Avg Total | Avg Prepare | Avg GetMeta RPC | Avg Peer Connect | Avg RDMA Prepare | Avg RDMA Get | Throughput |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 4KiB | 4 | 3569.75 us | 10.25 us | 402.00 us | 39.00 us | 28.75 us | 3059.50 us | 1.09 MB/s |
| 64KiB | 4 | 3480.50 us | 14.25 us | 648.25 us | 30.75 us | 22.50 us | 2738.25 us | 17.96 MB/s |
| 1MiB | 4 | 5380.25 us | 139.25 us | 404.75 us | 36.00 us | 21.00 us | 4642.00 us | 185.86 MB/s |

Notes from this VM benchmark pass:

- Publish is stable across the full `4KiB .. 128MiB` sweep.
- Fetch is stable for `4KiB`, `64KiB`, and `1MiB`.
- Fetch at `4MiB+` showed severe slowdown or timeout in the QEMU + Soft-RoCE
  environment and was not treated as a valid benchmark result.
- The most plausible interpretation is that this is an environment or UCX
  interaction issue rather than a simple `kv_bench` logic failure.
- Real NIC validation should re-run `fetch 4MiB+` before drawing any product
  conclusion about large-message one-sided read throughput.

### Acceptance Criteria

Functional acceptance on real RDMA hardware:

- `publish -> fetch` works
- `push -> fetch` works
- `unpublish` removes the key from future fetches
- no UCX assertions
- no verbs or connection-manager errors

Initial performance acceptance:

- `4 KiB`, `64 KiB`, and `1 MiB` payloads each complete successfully
- metrics are produced for publish, fetch, and push
- no environment-specific workaround is required unless the real environment
  reproduces the known Soft-RoCE issue
