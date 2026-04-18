# Release Verify Design

**Date:** 2026-04-18

**Goal:** Add a single entrypoint that can build release packages and run release validation end-to-end before shipping a version.

**Non-goals:**
- Replace the existing `x86_64` package build script.
- Add full CI automation in this change.
- Turn Soft-RoCE validation into a final performance authority.

## Requirements

The release verification flow must:

- build the latest `x86_64` package locally
- build the latest `aarch64` package on VM1
- pin every build and validation step to one explicit git commit
- validate packaged runtime integrity for both architectures
- run ARM package validation on VM1/VM2 with the existing Soft-RoCE setup
- run ALPS end-to-end validation on VM1/VM2
- run a minimal performance spot check
- run regression checks for existing examples so new functionality does not break old interfaces
- write logs and a machine-readable summary to a per-commit output directory
- stop on the first failure while preserving logs and artifacts

## Default Scope

The default release verification run covers:

1. local `x86_64` package build and package checks
2. remote `aarch64` package build on VM1
3. VM1/VM2 Soft-RoCE readiness checks
4. packaged runtime smoke for ARM binaries
5. `alps_kv_bench` end-to-end validation across VM1 and VM2
6. minimal performance matrix on ARM Soft-RoCE
7. example regression validation for:
   - `ping_pong`
   - `rdma_put_get`
   - `kv_demo`
   - `kv_wait_fetch`
   - `message_kv_demo`
   - `alps_kv_bench`

## Approach Options

### Option 1: Single Bash Script

Put all build, SSH, validation, log collection, and summary logic in one large shell script.

**Pros**
- fastest to start
- one file to run

**Cons**
- hard to maintain as validation grows
- easy to mix build and verification concerns
- harder to test in pieces

### Option 2: Orchestrator Plus Reusable Subscripts

Keep existing build scripts, add a reusable ARM remote build script, add a reusable example validation script, and add one top-level orchestrator.

**Pros**
- clean separation between build and verification
- easiest to extend safely
- best fit for current repo structure and existing scripts

**Cons**
- introduces a few new files

### Option 3: Python Orchestrator

Move orchestration into Python and shell out for builds and SSH calls.

**Pros**
- better structured state handling
- easier JSON generation and richer reporting

**Cons**
- larger tooling shift than needed right now
- duplicates existing shell-oriented packaging flow

## Recommended Approach

Use **Option 2**.

This keeps the current package scripts as the source of truth, minimizes risk to the existing release flow, and gives us clear seams for future extension. The new top-level release verification entrypoint should orchestrate the work, not reimplement it.

## Architecture

### 1. Entrypoint

Add `scripts/release_verify.sh` as the only user-facing command for release build verification.

It will:

- resolve one exact target commit and output directory
- invoke the local `x86_64` package build flow
- invoke the remote ARM package build flow
- check VM readiness
- run smoke, end-to-end, example regression, and perf spot-check steps
- write `summary.txt` and `summary.json`
- stop on first failure and report where logs were written

Commit identity is a hard invariant:

- if `--commit <sha>` is provided, every build and validation step must use that SHA
- if `--commit` is omitted, the script may use the current `HEAD` only when the tree is clean
- dirty or ambiguous source state must fail closed instead of silently verifying the wrong revision

Both the local `x86_64` package flow and the remote ARM package flow must therefore accept a target SHA instead of always packaging the caller's current `HEAD`.

### 2. ARM Remote Build Worker

Add `scripts/build_pkg_arm_remote.sh`.

It will:

- archive one explicit commit SHA, not an implicit moving `HEAD`
- copy source and `ucx-v1.20.0.tar.gz` to VM1
- build the `aarch64` package on VM1 using the same packaging shape as current manual remote flow
- leave the package on VM1 for validation
- optionally copy the resulting tarball back to `out/packages/` so local artifacts stay complete

This script should centralize the remote ARM package logic that is currently ad hoc and partially duplicated.

### 3. Example Validation Worker

Add `scripts/release_verify_examples.sh`.

It will:

- stage the exact target commit onto VM1 and VM2 in throwaway build trees
- build only the example binaries needed for regression on each VM
- run the selected example regressions from those build trees
- store logs under the orchestrator output directory
- return non-zero on the first failed example

The intent is to keep release regression commands and log paths separate from the main orchestration logic. Example regression does not rely on packaged binaries except where the package actually ships the target, because the current release package only installs `zerokv`, `alps_kv_wrap`, and `alps_kv_bench`.

### 4. Perf Matrix Reuse

Reuse `scripts/perf_experiments.py` for the minimal performance spot-check instead of reimplementing matrix logic.

The release flow should run only a constrained default matrix:

- sizes: `1K,1M,32M`
- environment:
  - `UCX_PROTO_ENABLE=n`
  - `UCX_MAX_RMA_RAILS=1`
  - `UCX_TLS=rc,sm,self`
  - `UCX_NET_DEVICES=rxe0:1`

This default is intentionally specific to the current VM Soft-RoCE setup, where the documented workaround is to disable the UCX new protocol stack. This step is a regression signal only, not a final performance certification.

## Validation Layers

### Package Integrity

#### `x86_64`

Delegate to `scripts/build_pkg_x86_compile.sh`, which already:

- builds in a `glibc 2.28` container
- stages packaged UCX runtime and providers
- checks runtime dependency resolution
- checks UCX runtime presence
- checks GLIBC symbol floor

#### `aarch64`

The remote ARM build script must check:

- package contains `COMMIT_ID`, `ARCH`, `README.md`
- package contains `bin/alps_kv_bench`, `bin/ucx_info`
- package contains `lib/libalps_kv_wrap.so`, `lib/libzerokv.so`
- package contains `lib/ucx/` provider modules
- no unexpected `GLIBCXX_*` requirement regression
- compiler/toolchain fingerprint is recorded in logs

### Runtime Smoke

All ARM validation commands that run on the VM Soft-RoCE pair must inherit one shared environment profile before any binary is launched:

- `UCX_PROTO_ENABLE=n`
- `UCX_NET_DEVICES=rxe0:1`
- `UCX_TLS=rc,sm,self`

#### Local `x86_64`

Run unpacked:

- `bin/ucx_info -d`
- `bin/alps_kv_bench --mode server ...`

#### ARM on VM1/VM2

Run unpacked:

- `bin/ucx_info -d`
- `bin/alps_kv_bench --mode server ...`

These checks ensure the package loads, UCX modules resolve, and ALPS listener startup still works.

### Soft-RoCE Readiness

Before ARM end-to-end validation, verify on both VMs:

- `ibv_devices` shows `rxe0`
- `ucx_info -d` exposes RDMA-capable transports
- `rdma link show` or equivalent sysfs inspection shows which netdev backs `rxe0`
- the exact server/client RDMA IPs chosen for validation belong to that same netdev
- the expected RDMA IPs are reachable over that interface

Fail early if the simulated RDMA environment is not ready or if the advertised address does not match the NIC selected by `UCX_NET_DEVICES`.

### End-to-End ALPS Validation

Run one packaged `alps_kv_bench` server on VM1 and one packaged client on VM2.

Both sides must use the shared ARM Soft-RoCE environment profile.

Pass criteria:

- server starts and prints `ALPS_KV_LISTEN`
- client completes and prints `ALPS_KV_ROUND`
- the new timing fields are present

### Example Regression Validation

Default example regression set:

- `ping_pong`
- `rdma_put_get`
- `kv_demo`
- `kv_wait_fetch`
- `message_kv_demo`
- `alps_kv_bench`

These checks protect the previously used interfaces and sample entrypoints from regressions caused by newer functionality.
`alps_kv_bench` should run from the packaged artifact path; the other examples should run from throwaway build trees created from the same pinned commit on the target VMs.
All example commands on VM1/VM2 must also use the shared ARM Soft-RoCE environment profile.

### Performance Spot Check

Run the constrained ARM Soft-RoCE matrix through `scripts/perf_experiments.py`.

The perf worker should start from the shared ARM Soft-RoCE environment profile and then add any perf-specific knobs on top.

Artifacts must include:

- `manifest.json`
- `summary.csv`
- raw server/client logs

The release summary should record whether this spot-check passed, not interpret performance deeply.

## Output Layout

All logs for a run live under:

`out/release-verify/<commit>/`

Expected layout:

```text
out/release-verify/<commit>/
  summary.txt
  summary.json
  x86/
    build.log
    package.txt
    runtime/
      ucx_info.log
      alps_server.log
  arm/
    build.log
    package.txt
    vm1/
      ucx_info.log
      alps_server.log
    vm2/
      ucx_info.log
    e2e/
      alps_server.log
      alps_client.log
    examples/
      ping_pong/
      rdma_put_get/
      kv_demo/
      kv_wait_fetch/
      message_kv_demo/
      alps_kv_bench/
    perf/
      manifest.json
      summary.csv
```

`summary.json` should use a stable minimal schema:

- `commit`: validated git SHA
- `started_at`
- `finished_at`
- `packages`: object keyed by architecture, with artifact path and status
- `steps`: ordered list of objects containing:
  - `name`
  - `status`: `pass|fail|skip`
  - `duration_ms`
  - `log_path`
  - `artifact_path` when relevant
  - `reason` for failures and skips

## CLI

The top-level entrypoint should support:

```bash
scripts/release_verify.sh \
  [--commit <sha>] \
  [--skip-x86] \
  [--skip-arm] \
  [--skip-perf] \
  [--skip-examples] \
  [--vm1 192.168.3.9:2222] \
  [--vm2 192.168.3.9:2223] \
  [--vm-user axon] \
  [--vm-pass axon]
```

Defaults should match the current environment:

- VM1: `192.168.3.9:2222`
- VM2: `192.168.3.9:2223`
- user/password: `axon/axon`

If `--commit` is supplied, the summary and both package metadata files must report that exact SHA. If it is omitted, the script must resolve one SHA up front and reuse it everywhere.

## Failure Model

- stop on the first failed step
- preserve all generated logs and artifacts
- print the failed step and the relevant log path
- still emit `summary.txt` and `summary.json`

Example human summary:

```text
PASS x86 package build
PASS x86 runtime smoke
PASS arm package build
PASS arm soft-roce ready
PASS arm alps e2e
PASS arm examples: ping_pong
FAIL arm examples: kv_demo log=out/release-verify/<commit>/arm/examples/kv_demo/client.log

x86 package: out/packages/alps_kv_wrap_pkg-x86_64-<commit>.tar.gz
arm package: out/packages/alps_kv_wrap_pkg-aarch64-<commit>.tar.gz
```

## Testing Strategy

Use TDD for the new orchestration code.

### Automated tests

Add small focused tests for:

- top-level argument parsing
- summary generation
- dry-run command rendering for example validation

These tests should avoid requiring real VMs.

### Manual verification

Final acceptance requires one real run against:

- local `x86_64` package build path
- VM1 ARM build path
- VM1/VM2 Soft-RoCE validation path

## Risks

- SSH and password-driven remote flows are fragile if VM access changes
- Soft-RoCE validates behavior trends, not true hardware performance
- example regression commands may need careful timeout handling to avoid false failures
- package validation logic must not drift from the actual package layout again

## Success Criteria

The work is complete when:

- one command can build and validate both release packages
- both package paths are recorded in the final summary
- the same pinned commit SHA is used across local build, remote build, runtime validation, and regression validation
- example regressions run from the correct source of truth:
  - packaged artifacts for shipped package entrypoints
  - throwaway build trees for examples not installed into the release package
- ARM validation uses VM1/VM2 Soft-RoCE directly
- failures are obvious and actionable from preserved logs
