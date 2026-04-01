# MessageKV Size Sweep Demo Design

## Summary

Extend `examples/message_kv_demo.cpp` from a single-round correctness demo into a
two-node size-sweep demo for the first real MessageKV scenario:

- `RANK0`: one process colocating `KVServer + MessageKV receiver`
- `RANK1`: one sender process
- each round uses 4 sender threads
- each sender thread sends exactly 1 unique key
- the sender main thread waits for all 4 threads to finish before advancing to
  the next round
- sizes are user-configurable via `--sizes`

This remains a demo/example, not a new benchmark binary. The goal is to make it
easy to unpack the source tarball, build, and run a realistic multi-round test
on two machines.

## Goals

- Add configurable size sweep support to `message_kv_demo`
- Preserve the current two-node deployment model
- Keep receiver configuration simple: no receiver-side thread-count argument
- Make each round easy to read from stdout with one sender summary line and one
  receiver summary line
- Keep the per-round traffic pattern aligned with the target scenario:
  4 concurrent sends from `RANK1` to `RANK0`

## Non-Goals

- No new protocol changes
- No new benchmark executable
- No CSV output in this change
- No infinite streaming mode
- No changes to MessageKV semantics beyond what is needed to drive the demo

## CLI Shape

### Common

Add:

- `--sizes <csv>`

Default:

- `1K,64K,1M,4M,16M,32M,64M,128M`

Accepted syntax should reuse the existing size parser pattern already used by
`kv_bench` semantics in the repository:

- `K`, `M`, `G`
- case-insensitive

### RANK0

Keep:

- `--role rank0`
- `--listen`
- `--data-addr`
- `--node-id`
- `--timeout-ms`
- `--post-recv-wait-ms`
- `--transport`

Keep:

- `--messages`

Default:

- `4`

Validation:

- `--messages > 0`
- `--sizes` non-empty

### RANK1

Keep:

- `--role rank1`
- `--server-addr`
- `--data-addr`
- `--node-id`
- `--threads`
- `--transport`

Default:

- `--threads 4`

Validation:

- `--threads > 0`
- `--sizes` non-empty

Phase 1 assumes the intended scenario shape is:

- `messages == threads == 4`

The demo may allow other positive values, but the README should document `4` as
the scenario-aligned setting.

## Round Model

For each configured size in `--sizes`:

1. `RANK1` creates 4 payloads of exactly that size
2. `RANK1` launches 4 sender threads
3. each thread sends exactly 1 unique key
4. sender main thread waits for all 4 threads to finish
5. only after all 4 sends finish does the sender advance to the next size

In parallel on `RANK0`:

1. for the current round, prepare 4 expected keys
2. allocate one shared receive region sized for `messages * size`
3. issue 4 receives, one per expected key
4. wait until the round completes
5. print round summary
6. optionally sleep `post_recv_wait_ms`
7. move to the next size

The round index is part of the logical message identity so rounds cannot collide
with each other.

## Key Convention

The demo continues to generate its own keys only as a deterministic example.
This does not change the broader MessageKV rule that real business code owns key
uniqueness.

Per-round demo key format:

- `msg-round<round>-size<size>-thread<thread>`

Properties:

- unique across rounds
- unique across sender threads within a round
- deterministic on both `RANK0` and `RANK1`

## Payload Convention

Each sender thread fills a payload of exactly the round size.

Format:

- a short textual prefix containing round/thread metadata
- the remainder padded with deterministic bytes

This keeps validation simple:

- receiver can verify exact byte length
- receiver can print a short prefix preview without dumping large payloads

The demo should avoid printing full payloads for large sizes. For large rounds,
stdout should print:

- key
- payload size
- a short preview prefix

## Receiver Execution Model

Receiver should continue to avoid a receiver-side `--threads` argument.

Implementation approach:

- use `args.messages` to determine how many receives to issue each round
- use one `MessageKV` receiver instance per in-flight receive if needed to avoid
  wrapper-level serialization
- this remains an internal implementation detail, not a public CLI contract

The public contract is only:

- `RANK0` receives `messages` keys per round

## Output Format

### Sender Per-Round Summary

Print one summary line per size:

- `SEND_ROUND`
- `round`
- `size`
- `messages`
- `send_total_us`
- `max_thread_send_us`
- `total_bytes`
- `throughput_MiBps`

Optional per-thread lines may remain, but the summary line is the primary
machine-readable output for real-environment validation.

### Receiver Per-Round Summary

Print one summary line per size:

- `RECV_ROUND`
- `round`
- `size`
- `completed`
- `failed`
- `timed_out`
- `completed_all`
- `recv_total_us`
- `total_bytes`
- `throughput_MiBps`

Per-message detail should be compact:

- for small sizes, printing payload preview is acceptable
- for large sizes, print key + byte count + short preview only

## Error Handling

- invalid or empty `--sizes` is a startup error
- invalid size token is a startup error
- sender aborts immediately if any thread fails in a round
- receiver aborts immediately if a round fails
- mismatch in expected payload length is treated as a round failure

No partial-round retry logic is added in this change.

## README Changes

Update the MessageKV demo section to describe:

- size sweep mode
- default sizes
- round semantics
- `RANK0` colocates server and receiver
- `RANK1` runs 4 sender threads per round

Document the recommended two-node commands using:

- `--sizes 1K,64K,1M,4M,16M,32M,64M,128M`
- `--messages 4`
- `--threads 4`

Also keep the existing warning:

- `UCX_NET_DEVICES` should be set explicitly
- `--data-addr` must match the selected NIC/IP plane

## Testing

At minimum:

1. extend the demo build so it still compiles cleanly
2. local/VM smoke test with small sizes such as `1K,64K`
3. verify sender prints one `SEND_ROUND` line per configured size
4. verify receiver prints one `RECV_ROUND` line per configured size
5. verify round keys do not collide across sizes/rounds

## Risks

- Printing large payloads would make logs unusable, so output must be truncated
- Sender and receiver must agree on the same round/key derivation
- Large receive regions may increase memory pressure on constrained VMs

## Recommendation

Implement this as an extension of `message_kv_demo`, not a separate benchmark.
This keeps the user-facing path simple:

- unpack tarball
- build `message_kv_demo`
- run two commands on two nodes
- observe per-round results across the size sweep
