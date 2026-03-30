# KV Direct Put Design

**Date:** 2026-03-30

## Goal

Add a first MVP+1 push path that lets one node send a `key -> value` directly
to a specified target node using RDMA put, after which the target node stores
the received value as a normal locally published KV object.

## Scope

In scope:

- new `KVNode::push(target_node_id, key, data, size)` API
- target-side fixed push inbox region created at node startup
- server-side registry for per-node push inbox metadata
- sender-side control-plane lookup of target push inbox metadata
- sender-side RDMA put into the target inbox
- control-plane `PUSH_COMMIT` so the target can finalize and publish the key
- tests for success and basic failure paths

Out of scope:

- dynamic per-push receive allocation
- zero-copy target-side final storage
- multi-slot inbox queues
- concurrent push arbitration beyond one in-flight push per target inbox
- subscription and push event fan-out
- benchmark mode or advanced performance reporting

## Product Semantics

The public API is business-oriented rather than transport-oriented:

```cpp
axon::Future<void> push(const std::string& target_node_id,
                        const std::string& key,
                        const void* data,
                        size_t size);
```

Behavior:

1. caller specifies the destination node directly
2. sender writes the payload into the target node's RDMA push inbox
3. sender issues a control-plane commit RPC
4. target finalizes the push and stores the key as a normal local published KV
   object
5. future readers can fetch the key from the target through the existing
   metadata + RDMA get path

This phase intentionally keeps the existing `fetch(key)` flow as the read path.
Push only changes how data is created on the destination node.

## Recommended Approach

### Option A: Fixed inbox + commit RPC

Each node owns one pre-registered push inbox buffer:

- created during `KVNode::start()`
- registered once
- advertised to the server as part of node registration

Push flow:

1. sender asks server for target push inbox metadata
2. sender RDMA-puts one framed message into the inbox
3. sender sends `PUSH_COMMIT`
4. target copies data from inbox into a normal published `MemoryRegion`
5. target updates its local published map and server metadata

Pros:

- minimal moving parts
- no per-push receive allocation protocol
- keeps server as routing/control authority only
- easiest path to a testable MVP+1

Cons:

- only one effective push slot per target inbox
- needs explicit size checks and busy handling
- target copies inbox bytes into final storage

### Option B: Dynamic receive-region negotiation

Target allocates a fresh region per push attempt and returns `remote_addr/rkey`
to the sender.

Pros:

- closer to a production push protocol
- avoids fixed inbox capacity constraints

Cons:

- much more protocol complexity
- harder failure handling
- not needed for first usable push feature

### Option C: Control-plane fallback push over TCP

Use TCP for payload transfer and skip RDMA put.

Pros:

- fastest implementation

Cons:

- misses the actual product goal
- creates a different semantic path from the existing RDMA design

## Chosen Design

Use **Option A**.

The first direct-put phase should prove the destination-directed push model with
the simplest RDMA-capable protocol. Dynamic inbox allocation can be a later
upgrade without changing the public `push()` API.

## Data Model Changes

### Server-side node metadata

Extend node registration state with push inbox metadata:

- `push_inbox_data_addr`
  - normally identical to node data-plane address
- `push_inbox_remote_addr`
- `push_inbox_rkey`
- `push_inbox_capacity`

This metadata belongs to the node, not to a specific key.

### Target-side inbox

Each `KVNode` owns:

- one fixed `MemoryRegion::Ptr push_inbox_region`
- packed `RemoteKey push_inbox_rkey`
- inbox capacity constant
- inbox state for simple single-slot arbitration:
  - idle
  - busy

The inbox layout should be:

```text
[PushInboxHeader][key bytes][payload bytes]
```

Where `PushInboxHeader` contains:

- protocol version
- key length
- value length

The sender writes header + key + payload in one contiguous RDMA put.

## Protocol Changes

Add new message types:

- `GET_PUSH_TARGET`
- `GET_PUSH_TARGET_RESP`
- `PUSH_COMMIT`
- `PUSH_COMMIT_RESP`

### GET_PUSH_TARGET

Request:

- `target_node_id`

Response:

- status
- `target_node_id`
- `target_data_addr`
- `push_inbox_remote_addr`
- `push_inbox_rkey`
- `push_inbox_capacity`
- message

### PUSH_COMMIT

Request:

- `target_node_id`
- `sender_node_id`
- `key`
- `value_size`

Response:

- status
- message

The commit request does not need to include `remote_addr/rkey`; those are only
needed by the sender for the RDMA put stage.

## Push Flow

### Sender

1. validate input
2. `GET_PUSH_TARGET(target_node_id)`
3. verify framed push message fits in target inbox capacity
4. get or connect peer endpoint to target data address
5. build `[header][key][payload]` buffer locally
6. RDMA `put` the framed message to `push_inbox_remote_addr`
7. send `PUSH_COMMIT`
8. return success only if commit succeeds

### Target

1. node starts with a registered inbox region
2. node registration advertises inbox metadata to server
3. upon `PUSH_COMMIT`, server routes or validates the request against the
   target node
4. target finalizes by:
   - parsing inbox header
   - validating key/value sizes and request consistency
   - allocating a new local `MemoryRegion`
   - copying payload bytes into the new region
   - publishing locally under the given key
   - updating server metadata through the existing publish path
5. target clears inbox busy state and acknowledges commit

## Concurrency And Simplifications

This phase intentionally allows only one active push into a target inbox at a
time.

Rules:

- if target inbox is busy, `GET_PUSH_TARGET` or `PUSH_COMMIT` should fail with a
  busy-style error
- sender does not retry automatically
- no multi-producer queueing in this phase

This keeps the initial push protocol understandable and testable.

## Error Handling

Expected failures:

- target node does not exist
- target node is offline
- framed message exceeds inbox capacity
- target inbox is busy
- RDMA put fails
- commit arrives but inbox contents are malformed

Error policy:

- sender returns the original failure status
- target must not publish partial data
- server should not expose the pushed key unless commit finalization succeeds

## Testing Strategy

Required tests:

1. push succeeds from sender to target
2. pushed key becomes fetchable through the existing `fetch(key)` path
3. push fails when target node is unknown
4. push fails when payload exceeds inbox capacity
5. push to one target does not corrupt normal publish/fetch behavior

Phase-1 style integration tests are sufficient; cross-VM validation can follow
after local integration tests are green.

## Risks And Trade-offs

- fixed inbox capacity limits the first implementation
- commit RPC adds one extra control-plane hop but keeps data visibility explicit
- single-slot inbox means no concurrent push throughput claims yet
- target-side finalization copies data into a new region, so this is not a
  zero-copy receive design

These trade-offs are acceptable because the goal is to validate the targeted
push model, not to finalize the long-term push transport protocol.
