# KV Publish/Fetch Benchmark Design

**Date:** 2026-03-30

## Goal

Add a dedicated `kv_bench` executable for two-node publish and fetch
benchmarks, with size-sweep execution and concise tabular output that is ready
to run on real RDMA hardware.

## Scope

In scope:

- a new `kv_bench` executable
- `server`, `hold-owner`, `bench-publish`, and `bench-fetch` modes
- size-sweep execution across a fixed default size list
- support for both `--iters` and `--total-bytes`
- human-readable table output for publish and fetch benchmarks
- light local coverage for size parsing and iteration calculation

Out of scope:

- push benchmarking
- subscription benchmarking
- Google Benchmark integration
- percentile or histogram reporting
- CSV or JSON export in this phase
- multi-owner orchestration inside the benchmark binary
- benchmark result persistence

## Topology

The benchmark is explicitly two-node plus metadata server:

- `server`
  - runs `KVServer`
  - serves registration and metadata lookups
- `owner`
  - runs `kv_bench --mode hold-owner`
  - publishes fixed keys for each benchmark size and remains alive
- `bench client`
  - runs `bench-publish` or `bench-fetch`

This keeps fetch measurements aligned with real deployment:

- a stable remote owner node
- a separate benchmark client node
- no in-process preparation hidden inside the fetch benchmark path

## User-Facing Design

Add a new executable:

```text
kv_bench
```

Supported modes:

- `server`
- `hold-owner`
- `bench-publish`
- `bench-fetch`

### CLI

Server:

```bash
./kv_bench --mode server --listen 10.0.0.1:15000 --transport rdma
```

Hold owner:

```bash
./kv_bench --mode hold-owner \
  --server-addr 10.0.0.1:15000 \
  --data-addr 10.0.0.2:0 \
  --node-id owner \
  --sizes 4K,64K,1M,4M,16M,32M,64M,128M \
  --transport rdma
```

Publish benchmark:

```bash
./kv_bench --mode bench-publish \
  --server-addr 10.0.0.1:15000 \
  --data-addr 10.0.0.3:0 \
  --node-id bench-pub \
  --sizes 4K,64K,1M,4M,16M,32M,64M,128M \
  --total-bytes 1G \
  --transport rdma
```

Fetch benchmark:

```bash
./kv_bench --mode bench-fetch \
  --server-addr 10.0.0.1:15000 \
  --data-addr 10.0.0.3:0 \
  --node-id bench-fetch \
  --sizes 4K,64K,1M,4M,16M,32M,64M,128M \
  --total-bytes 1G \
  --transport rdma
```

### Default Size Sweep

If `--sizes` is omitted, use:

- `4KiB`
- `64KiB`
- `1MiB`
- `4MiB`
- `16MiB`
- `32MiB`
- `64MiB`
- `128MiB`

### Iteration Control

Both control styles are supported:

- `--iters N`
- `--total-bytes SIZE`

Rules:

- if `--iters` is provided, use the same iteration count for every size
- otherwise compute iterations from `--total-bytes / size`
- computed iterations are clamped to at least `1`
- default `--total-bytes` is `1GiB`

This keeps large sizes tractable while still allowing predictable fixed-count
runs.

## Benchmark Semantics

### Hold Owner

`hold-owner` prepares the fetch benchmark dataset.

For each benchmark size:

- allocate a deterministic payload of the requested size
- publish it under a stable key
- keep the node alive until interrupted

Stable key format:

```text
bench-fetch-<size-bytes>
```

Example:

```text
bench-fetch-4096
bench-fetch-67108864
```

### Bench Publish

For each size:

- compute iteration count
- generate one deterministic payload buffer of that size
- for each iteration:
  - publish under a unique key
  - wait for completion
  - read `last_publish_metrics()`
  - unpublish the same key

Unique key format:

```text
bench-publish-<size-bytes>-<iter>
```

This avoids metadata overwrite effects and prevents metadata accumulation on
the server.

### Bench Fetch

For each size:

- compute iteration count
- fetch the corresponding stable owner key
- wait for completion
- validate the returned payload size
- read `last_fetch_metrics()`

Owner keys are provided by the independent `hold-owner` process.

`--owner-node-id` is optional. The benchmark always resolves the owner through
normal server metadata lookup. When `--owner-node-id` is provided, it is used
only as a sanity check that the fetched owner matches the expected topology.

## Output

Each benchmark mode prints one table.

Publish table columns:

- `size`
- `iters`
- `bytes`
- `avg_total_us`
- `avg_prepare_us`
- `avg_pack_rkey_us`
- `avg_put_meta_rpc_us`
- `throughput_MBps`

Fetch table columns:

- `size`
- `iters`
- `bytes`
- `avg_total_us`
- `avg_prepare_us`
- `avg_get_meta_rpc_us`
- `avg_peer_connect_us`
- `avg_rdma_prepare_us`
- `avg_rdma_get_us`
- `throughput_MBps`

Throughput is computed as:

```text
(size_bytes / avg_total_seconds) / (1024 * 1024)
```

Only averages are reported in this phase.

## Parsing Rules

`--sizes` accepts a comma-separated list with suffixes:

- `K`, `KB`, `KiB`
- `M`, `MB`, `MiB`
- `G`, `GB`, `GiB`

The parser should normalize everything to bytes using binary units.

Examples:

- `4K` -> `4096`
- `32M` -> `33554432`
- `1G` -> `1073741824`

Invalid tokens must fail fast with a clear error message.

## Internal Design

Add:

- `examples/kv_bench.cpp`

Reuse:

- `KVServer`
- `KVNode`
- `PublishMetrics`
- `FetchMetrics`

Small helper functions inside `kv_bench.cpp` are sufficient in this phase:

- parse size strings
- derive iteration count
- build deterministic payloads
- print aligned tables

There is no need to introduce a shared benchmark library yet.

## Error Handling

Benchmarking should fail fast on setup problems:

- invalid CLI arguments
- invalid size tokens
- owner preparation failure
- fetch size mismatch
- publish or fetch operation failure

For per-size execution:

- if an iteration fails, abort the benchmark and print the failing size and
  iteration index
- do not silently skip failed samples

This keeps benchmark results trustworthy.

## Testing Strategy

Add light coverage for:

1. parsing a mixed size list like `4K,64K,1M,32M`
2. converting `--total-bytes` to iterations with min-1 behavior
3. a small local benchmark smoke path that exercises at least one size and
   emits one table row

The smoke path should stay small enough for local CI-style execution and does
not need to validate numeric performance.

## Real RDMA Usage

Expected real-hardware workflow:

1. start `kv_bench --mode server`
2. start `kv_bench --mode hold-owner` on the owner host
3. run `kv_bench --mode bench-fetch` from the benchmark client host
4. run `kv_bench --mode bench-publish` from the benchmark client host

Recommended first pass:

- transport `rdma`
- default size sweep
- `--total-bytes 1G`

This is enough to identify where latency stops being control-plane dominated
and where throughput approaches NIC or fabric limits.

## Risks and Non-Goals

Known trade-offs in this phase:

- fetch benchmarking depends on a separately managed owner process
- short runs for very large sizes may produce noisy averages
- only averages are reported
- no CSV export yet
- no concurrent benchmark streams

This phase is for practical size-sweep validation on real RDMA hosts, not for
building a full benchmark framework.
