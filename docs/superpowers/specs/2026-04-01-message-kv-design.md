# Message KV Design

## Goal

Add a first scenario-focused wrapper layer named `message_kv` on top of ZeroKV.

The initial target scenario is:

- `RANK0` is the receiver
- `RANK1`, `RANK2`, and `RANK3` are senders
- each sender may use multiple threads
- senders concurrently publish multiple keys to the receiver
- the business layer already guarantees that message keys are unique

The wrapper should provide message-style send/receive APIs without exposing the
underlying subscription, fetch, or cleanup details to the business layer.

## Scope

In scope:

- Add a new `message_kv` wrapper layer above `zerokv::kv::KVNode`
- Use business-provided keys directly
- Provide sender-side APIs for copy and zero-copy publish
- Provide receiver-side APIs for single-key and batch receive
- Reuse the existing:
  - `publish()`
  - `publish_region()`
  - `fetch_to_many()`
  - `subscribe_and_fetch_to_once_many()`
  - `unpublish()`
- Keep message cleanup internal to the wrapper
- Use an internal ack-based reclaim path so sender-side published objects are
  eventually unpublished after successful consumption

Out of scope:

- Automatic key generation
- New server protocol messages
- Replacing the current wire format
- Owner-aware fetch scheduling
- Multi-NIC-aware message placement
- Reworking the current `push()` protocol into a multi-slot message queue
- End-to-end exactly-once delivery semantics
- Sender crash recovery or distributed garbage collection

## Why not use `push()` first

The current concrete workload is multi-producer to single-consumer:

- three sender ranks
- multiple sender threads
- all targeting a single receiver rank

The current `push()` path still centers on a single target inbox reservation.
It is safe after the reservation fixes, but it is not the right Phase 1
throughput shape for many concurrent senders hitting one receiver.

For this first scenario, `publish/publish_region + subscribe_and_fetch_to_*`
is the better base because:

- it avoids the target-side single inbox bottleneck
- it already matches the current mature APIs
- it lets the receiver consume directly into caller-provided memory
- it minimizes protocol risk for the first business integration

## API shape

Phase 1 should add a new wrapper type in a dedicated header/source pair.

Suggested location:

- `include/zerokv/message_kv.h`
- `src/message_kv.cpp`

Suggested primary type:

```cpp
namespace zerokv {

class MessageKV {
public:
    struct BatchRecvItem {
        std::string key;
        size_t length = 0;
        size_t offset = 0;
    };

    struct BatchRecvResult {
        std::vector<std::string> completed;
        std::vector<std::string> failed;
        std::vector<std::string> timed_out;
        bool completed_all = false;
    };

    static std::shared_ptr<MessageKV> create();

    void start(const kv::NodeConfig& cfg);
    void stop();

    void send(const std::string& key, const void* data, size_t size);
    void send_region(const std::string& key,
                     const zerokv::MemoryRegion::Ptr& region,
                     size_t size);

    void recv(const std::string& key,
              const zerokv::MemoryRegion::Ptr& region,
              size_t length,
              size_t offset,
              std::chrono::milliseconds timeout);

    BatchRecvResult recv_batch(const std::vector<BatchRecvItem>& items,
                               const zerokv::MemoryRegion::Ptr& region,
                               std::chrono::milliseconds timeout);
};

}  // namespace zerokv
```

The exact type names can still be refined during implementation, but the public
shape should stay in this family:

- sender APIs speak in message terms
- receiver APIs speak in key + buffer placement terms
- ack is not visible in the public API

## Ownership model

Phase 1 keeps sender-side ownership for the message payload.

That means:

- `send()` uses `publish()`
- `send_region()` uses `publish_region()`
- the sender node remains the owner of the published key
- the receiver fetches the message into its own memory
- sender-side cleanup happens only after successful receiver acknowledgement

This is intentionally different from a target-owned `push()` flow.

## Internal ack model

The ack mechanism is an internal implementation detail.

The business layer should not provide, inspect, or wait on ack keys directly.

Phase 1 internal flow:

1. sender publishes the message key
2. receiver receives the message successfully
3. receiver internally emits an ack key tied to that message key
4. sender-side background cleanup watches for that ack
5. sender-side background cleanup performs `unpublish(message_key)`
6. sender-side background cleanup removes the ack marker when safe

The important API rule is:

- successful receive eventually causes sender-side reclamation
- no ack-related surface leaks into the application API

## Key rules

The business layer is responsible for key uniqueness.

Phase 1 does not:

- derive keys from `(tag, index, src, dst)`
- append thread ids
- allocate sequence numbers
- enforce naming schemes

`message_kv` treats keys as opaque business identifiers.

If two senders publish the same key concurrently, the wrapper does not attempt
to resolve that conflict. That remains a caller contract violation.

## Sender semantics

### `send(key, data, size)`

- validates non-empty key and non-null data for non-zero size
- calls `KVNode::publish(key, data, size)`
- records that this key is now pending cleanup on the sender side
- returns after publish succeeds or throws on failure

### `send_region(key, region, size)`

- validates non-empty key
- validates non-null region
- validates `size <= region->length()`
- calls `KVNode::publish_region(key, region, size)`
- records that this key is now pending cleanup on the sender side
- returns after publish succeeds or throws on failure

Neither sender API waits for the receiver to consume the message.

## Receiver semantics

### `recv(key, region, length, offset, timeout)`

Single-key receive is a thin wrapper around the batch path:

- build one `BatchRecvItem`
- call `recv_batch(...)`
- success means the bytes are written into the caller region
- failure or timeout throws

### `recv_batch(items, region, timeout)`

Batch receive:

- validates the shared layout
- treats the input as a batch of expected message keys
- immediately fetches any keys already present
- subscribes and waits for missing keys
- fetches each key into the requested offset as soon as it becomes ready
- returns partial progress on timeout

Internally this should map directly to:

- `KVNode::subscribe_and_fetch_to_once_many(...)`

The receiver does not own sender-side published objects and therefore does not
call `unpublish(message_key)` directly.

## Cleanup behavior

Cleanup should be sender-driven and asynchronous.

Suggested internal components:

- a sender-side pending-message registry
- a sender-side background ack watcher
- a sender-side background unpublish worker

Phase 1 behavior:

- sender adds a key to the pending registry after successful publish
- receiver success generates an internal ack marker
- sender watcher consumes ack markers and queues cleanup
- cleanup worker performs `unpublish(message_key)`

Cleanup is best-effort in Phase 1:

- if unpublish fails transiently, the worker may retry
- if the process exits abruptly, pending published keys may remain until later
  manual or test cleanup

## Error handling

Public API should be synchronous and throw on direct operation failure, matching
the current helper style already used in `KVNode`.

Examples:

- invalid layout -> throw
- publish failure -> throw
- single-key receive timeout -> throw

For batch receive:

- return `BatchRecvResult`
- `completed` lists successful placements
- `failed` lists placements that reached terminal failure
- `timed_out` lists keys still pending when timeout expires
- `completed_all` is true only when every placement succeeds

This should mirror the behavior of `subscribe_and_fetch_to_once_many(...)`,
with message-oriented naming rather than raw KV naming.

## Concurrency expectations

Phase 1 should support:

- multiple sender processes
- multiple sender threads
- concurrent sends of different keys
- one receiver waiting on one or more keys

Phase 1 does not promise:

- fairness across senders
- bounded sender-side memory growth without ack progress
- optimized batching by owner or sender

Those are future improvements, not initial wrapper responsibilities.

## Testing

Phase 1 should cover at least:

1. sender `send()` + receiver `recv()` happy path
2. sender `send_region()` + receiver `recv()` zero-copy happy path
3. receiver `recv_batch()` with some keys already present
4. receiver `recv_batch()` with keys that appear later
5. batch timeout returns partial progress
6. sender-side cleanup eventually unpublishes after internal ack
7. duplicate keys in one receive batch are rejected only if the output layout is
   invalid; otherwise business-level duplicate-key semantics are caller-owned
8. stop/shutdown drains or safely stops internal cleanup workers

## Future evolution

Potential later phases:

- replace internal ack keys with a lighter internal control path
- introduce receiver-side multi-slot inbox / queue semantics
- owner-aware receive scheduling
- explicit send completion / delivery confirmation APIs
- sender crash cleanup strategy
- multi-NIC-aware message placement

Phase 1 intentionally does not solve those.
