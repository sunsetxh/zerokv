# MessageKV `send_region()` Demo Design

## Goal

Keep both public MessageKV send APIs:

- `send(key, data, size)` for simple owning-buffer usage
- `send_region(key, region, size)` for zero-copy / performance-sensitive usage

Then switch `examples/message_kv_demo.cpp` to use `send_region()` by default so the
size-sweep demo better reflects the performance ceiling of the MessageKV path,
especially for `1MiB+` payloads.

## In Scope

- Preserve both public MessageKV send APIs exactly as-is
- Change `message_kv_demo` sender rounds to:
  - pre-allocate one `MemoryRegion` per sender thread
  - reuse that region across warmup and measured rounds
  - rewrite the payload bytes in-place each round
  - call `send_region()` instead of `send()`
- Keep the current summary output format unchanged so before/after data stays
  directly comparable
- Update README examples or notes only if they describe sender behavior in a way
  that becomes stale after this change

## Out of Scope

- Changing the MessageKV public API surface
- Removing `send()`
- Adding new zero-copy aliases such as `send_zero_copy()`
- Changing receiver behavior
- Adding async send semantics
- Changing benchmark math or output columns

## Design

### Public API

No public API changes.

`MessageKV` continues to expose both:

- `send(...)`
- `send_region(...)`

The design intent is:

- `send(...)` remains the convenience path
- `send_region(...)` remains the performance path

### Demo Sender Path

For `rank1`, each sender thread should:

1. create its own `MessageKV` instance once
2. create its own reusable `MemoryRegion` once, sized to the maximum configured
   payload size
3. for each warmup/measured round:
   - generate the round payload bytes
   - copy them into the front of the reusable region
   - call `send_region(key, region, size_bytes)`

This keeps the current demo threading model unchanged while removing repeated
region allocation/registration from the hot path.

### Region Sizing

The reusable region length should be:

- `max(args.sizes)`

This guarantees all configured rounds fit without reallocation.

### Payload Handling

The demo should continue to use the existing deterministic payload generator so
receiver-side verification and compact previews remain unchanged.

Only the transport of the bytes changes:

- before: payload string -> `send()`
- after: payload string -> copy into reusable region -> `send_region()`

### Expected Effect

This change should mainly reduce sender-side overhead for `1MiB+` sizes by
removing repeated send-buffer setup costs from the measured loop.

It does **not** remove:

- publish / ack / cleanup semantics
- receiver-side fetch costs
- control-plane overhead

So the demo will still remain slower than `kv_bench --mode bench-fetch-to`.

## Testing

1. Existing `message_kv_demo` helper tests still pass unchanged
2. Existing `MessageKV` integration tests still pass unchanged
3. VM1/VM2 smoke:
   - TCP with `--sizes 1K,1M`
   - RDMA (`rxe0:1`) with `--sizes 1K,1M`
4. Compare measured sender-side rounds before/after on `1MiB+` sizes

## Risks

- The demo still does one local copy from generated payload bytes into the
  reusable region; this is acceptable because the goal is to avoid repeated
  region allocation/registration, not to eliminate all user-space copies in the
  demo harness.
- Large configured sizes increase per-thread reusable region memory footprint.
  With 4 sender threads and `100MiB` max size, the sender side will hold about
  `400MiB` of reusable send buffers.
