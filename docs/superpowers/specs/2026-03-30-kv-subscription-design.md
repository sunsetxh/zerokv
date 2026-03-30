# KV Subscription Design

**Date:** 2026-03-30

## Goal

Add a first subscription feature that lets nodes subscribe to key lifecycle
events from the server metadata plane without changing the current synchronous
server control connection model.

## Scope

In scope:

- exact-key subscription
- live event delivery only
- server as metadata event source
- server fan-out to subscriber-specific TCP listener
- subscriber-side local event queue
- events for:
  - publish
  - update
  - unpublish
  - owner loss
- polling-style event retrieval API on `KVNode`

Out of scope:

- wildcard or prefix subscriptions
- replay for late subscribers
- durable subscription state
- acknowledgement / redelivery
- ordering guarantees across different keys
- callback-based API
- control-plane transport rewrite for bidirectional server push on `control_fd_`

## Product Semantics

The subscription API should remain simple and polling-oriented:

```cpp
enum class SubscriptionEventType {
    kPublished,
    kUpdated,
    kUnpublished,
    kOwnerLost,
};

struct SubscriptionEvent {
    SubscriptionEventType type;
    std::string key;
    std::string owner_node_id;
    uint64_t version = 0;
};

axon::Future<void> subscribe(const std::string& key);
axon::Future<void> unsubscribe(const std::string& key);
std::vector<SubscriptionEvent> drain_subscription_events();
```

Behavior:

1. node subscribes to one exact key
2. server records the subscription
3. when metadata for that key changes, server notifies current subscribers
4. subscriber stores the event in a local in-memory queue
5. application polls with `drain_subscription_events()`

This is deliberately a pull-from-local-queue API, not a callback API.

## Key Constraint

Current `KVNode` uses a single synchronous `control_fd_` for request-response
RPCs (`register`, `put_meta`, `get_meta`, `unpublish`, `get_push_target`).
There is no background receive loop, request multiplexer, or server-initiated
message handling on that connection.

Therefore, the first subscription phase must **not** deliver notifications over
the existing `control_fd_`.

## Chosen Approach

Use a dedicated subscriber event listener on each node:

1. subscriber node starts a small TCP event listener
2. subscriber includes its `subscription_control_addr` during registration
3. `subscribe(key)` sends a normal request to server
4. when an event happens, server opens a short-lived TCP connection to the
   subscriber event listener and sends the event
5. subscriber enqueues the event locally and closes the connection

This preserves the current request-response control model while still letting
server act as the metadata event source.

## Alternatives Considered

### Option A: Reuse `control_fd_` for server push

Rejected for this phase.

Would require:

- background reader thread in `KVNode`
- request ID routing and pending RPC tracking
- refactor of all current synchronous RPC helpers

This is a larger control-plane redesign and should not be coupled to first-pass
subscription support.

### Option B: Polling-only subscriptions

Rejected.

The server would maintain "last changed version" and clients would poll.
Simple, but it does not satisfy the stated goal that server fan out metadata
events to interested subscribers.

### Option C: Dedicated subscriber listener

Chosen.

Minimal code expansion, keeps server as event source, and fits the current
control architecture.

## Data Model Changes

### Node registration

Extend node registration with:

- `subscription_control_addr`

### Server subscription registry

Server stores:

- `key -> set<subscriber_node_id>`

and resolves each subscriber node ID to its current registered
`subscription_control_addr`.

### Subscriber local state

Each `KVNode` owns:

- one TCP subscription listener
- one in-memory event queue
- a mutex for event queue push/pop

## Protocol Changes

Add these message types:

- `SUBSCRIBE`
- `SUBSCRIBE_RESP`
- `UNSUBSCRIBE`
- `UNSUBSCRIBE_RESP`
- `SUBSCRIPTION_EVENT`

### SUBSCRIBE

Request:

- `subscriber_node_id`
- `key`

Response:

- status
- message

### UNSUBSCRIBE

Request:

- `subscriber_node_id`
- `key`

Response:

- status
- message

### SUBSCRIPTION_EVENT

Payload:

- event type
- key
- owner node ID
- version

This message is server -> subscriber over the dedicated subscription listener.
It does not need a response in phase 1; delivery is best-effort.

## Event Source Rules

Server emits:

- `kPublished`
  - when a key first appears
- `kUpdated`
  - when an existing key is overwritten by the same owner with a higher version
- `kUnpublished`
  - when owner explicitly removes the key
- `kOwnerLost`
  - when owner disconnect causes the server to invalidate the key

The event should be emitted after server metadata state is updated.

## Delivery Semantics

Phase 1 is explicitly:

- best-effort
- live only
- no replay
- no ack / retry

If subscriber listener is unreachable, server drops the event and continues.

This must be documented clearly so callers do not infer durable notification
semantics.

## Server Responsibilities

Server remains the metadata authority and event source:

- record subscriptions
- remove subscriptions on explicit unsubscribe
- remove dead-node subscriptions when a subscriber disconnects
- fan out lifecycle events to current subscribers via short-lived TCP sends

Server does not persist events and does not queue undeliverable notifications.

## Subscriber Responsibilities

Subscriber node:

- starts a dedicated subscription listener
- registers `subscription_control_addr`
- sends `subscribe` / `unsubscribe` RPCs to server
- receives `SUBSCRIPTION_EVENT` messages
- appends them to the local queue
- exposes queue draining to the application

## Ordering Model

Phase 1 guarantees only:

- events for the same key are emitted by the server in metadata update order

Phase 1 does not guarantee:

- total ordering across different keys
- ordering relative to local application actions outside server metadata updates
- delivery if subscriber is temporarily unavailable

## Error Handling

Expected failures:

- subscribe to unknown or empty key name
- unsubscribe for a key that is not subscribed
- subscriber listener unavailable during event fan-out

Policy:

- subscribe/unsubscribe RPCs return explicit success/failure
- event fan-out failures are silent to the subscriber and only affect that
  event delivery
- subscriber queueing must not crash the node on malformed event payloads

## Testing Strategy

Required tests:

1. subscriber receives `published` event after another node publishes key
2. subscriber receives `updated` event after overwrite by same owner
3. subscriber receives `unpublished` event after explicit unpublish
4. subscriber receives `owner lost` event after publisher disconnect
5. unsubscribed node stops receiving future events

Tests should continue to run in single-VM integration first. Cross-VM validation
can follow after local behavior is stable.

## Risks And Trade-offs

- best-effort delivery means missed events are possible
- event queue is in-memory only
- dedicated subscription listener adds another address to node registration
- no wildcard subscriptions yet

These are acceptable trade-offs for the first subscription phase because the
goal is to validate metadata-driven event fan-out, not to build a durable
notification subsystem.
