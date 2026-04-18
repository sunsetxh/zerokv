# Release Verify Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a one-command release verification flow that builds the `x86_64` and `aarch64` packages, validates packaged runtime behavior, runs VM Soft-RoCE end-to-end checks, runs example regressions, and records machine-readable results for one pinned git commit.

**Architecture:** Keep the user-facing entrypoint in shell (`scripts/release_verify.sh`) and reuse the existing x86 package builder. Add one remote ARM build worker, one example-regression worker, and a small Python helper module for argument/state/summary handling so the orchestration remains testable. All release steps must run against one resolved commit SHA and write logs under `out/release-verify/<commit>/`.

**Tech Stack:** Bash, Python 3, existing package scripts, SSH/rsync/scp, VM1/VM2 Soft-RoCE environment, `scripts/perf_experiments.py`, `unittest`

---

## File Map

**Create:**
- `scripts/release_verify.sh`
  Entry-point orchestrator. Resolves commit, creates output directories, calls worker scripts, records step status, stops on first failure.
- `scripts/build_pkg_arm_remote.sh`
  Remote ARM package worker. Builds one explicit commit on VM1, stages the package on VM1, and optionally copies the tarball back to local `out/packages/`.
- `scripts/release_verify_examples.sh`
  VM1/VM2 example regression worker. Builds example binaries from throwaway per-commit build trees and runs the approved example set.
- `scripts/release_verify_lib.py`
  Small helper used by the shell scripts for commit resolution rules, summary JSON generation, and dry-run-friendly command rendering.
- `tests/python/test_release_verify.py`
  Focused tests for commit pinning, summary schema generation, and dry-run command building.

**Modify:**
- `scripts/build_pkg_x86_compile.sh`
  Accept a target commit SHA instead of always packaging the caller’s current `HEAD`. Fail on dirty trees when commit is omitted.
- `scripts/perf_experiments.py`
  Only if needed for cleaner release integration; prefer reusing the current CLI unchanged.
- `README.md`
  Add one short section showing how to run the new release verification entrypoint.

**Reference:**
- `docs/superpowers/specs/2026-04-18-release-verify-design.md`
- `scripts/qemu_rdma/run_test.sh`
- `scripts/build_pkg_x86_compile.sh`
- `scripts/perf_experiments.py`
- `CMakeLists.txt`

## Chunk 1: Commit Pinning And Summary Helper

### Task 1: Add tested commit-resolution and summary helpers

**Files:**
- Create: `scripts/release_verify_lib.py`
- Test: `tests/python/test_release_verify.py`

- [ ] **Step 1: Write the failing tests for commit resolution and summary schema**

```python
import importlib.util
import json
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT_PATH = ROOT / "scripts" / "release_verify_lib.py"


def load_module():
    spec = importlib.util.spec_from_file_location("release_verify_lib", SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class ReleaseVerifyLibTests(unittest.TestCase):
    def test_summary_includes_commit_packages_and_steps(self):
        module = load_module()
        summary = module.new_summary("abc123")
        module.record_step(
            summary,
            name="x86 package build",
            status="pass",
            duration_ms=12,
            log_path="out/release-verify/abc123/x86/build.log",
            artifact_path="out/packages/alps_kv_wrap_pkg-x86_64-abc123.tar.gz",
        )
        self.assertEqual(summary["commit"], "abc123")
        self.assertEqual(summary["steps"][0]["status"], "pass")
        self.assertIn("packages", summary)

    def test_resolve_target_commit_rejects_dirty_tree_without_explicit_commit(self):
        module = load_module()
        with self.assertRaisesRegex(RuntimeError, "dirty"):
            module.resolve_target_commit(
                head_sha="abc123",
                explicit_commit=None,
                worktree_dirty=True,
            )
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python3 -m unittest tests/python/test_release_verify.py -v`
Expected: FAIL with `ModuleNotFoundError` or missing helper functions.

- [ ] **Step 3: Write the minimal helper implementation**

```python
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def resolve_target_commit(head_sha: str, explicit_commit: str | None, worktree_dirty: bool) -> str:
    if explicit_commit:
        return explicit_commit
    if worktree_dirty:
        raise RuntimeError("dirty worktree requires --commit")
    return head_sha


def new_summary(commit: str) -> dict:
    return {
        "commit": commit,
        "started_at": utc_now(),
        "finished_at": None,
        "packages": {},
        "steps": [],
    }


def record_step(summary: dict, *, name: str, status: str, duration_ms: int,
                log_path: str | None = None, artifact_path: str | None = None,
                reason: str | None = None) -> None:
    summary["steps"].append({
        "name": name,
        "status": status,
        "duration_ms": duration_ms,
        "log_path": log_path,
        "artifact_path": artifact_path,
        "reason": reason,
    })
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python3 -m unittest tests/python/test_release_verify.py -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add scripts/release_verify_lib.py tests/python/test_release_verify.py
git commit -m "Add release verification helper library"
```

### Task 2: Thread target SHA through the x86 packager

**Files:**
- Modify: `scripts/build_pkg_x86_compile.sh`
- Test: `tests/python/test_release_verify.py`

- [ ] **Step 1: Add a failing test for commit-aware source archive selection**

```python
    def test_render_x86_archive_name_uses_explicit_commit(self):
        module = load_module()
        result = module.source_archive_name("x86_64", "deadbee")
        self.assertIn("deadbee", result)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `python3 -m unittest tests/python/test_release_verify.py -v`
Expected: FAIL with missing helper.

- [ ] **Step 3: Add the minimal helper and x86 script changes**

Implementation requirements:
- add helper `source_archive_name(arch, commit)`
- make `scripts/build_pkg_x86_compile.sh` accept `TARGET_COMMIT`
- when `TARGET_COMMIT` is set:
  - use `git archive TARGET_COMMIT`
  - use `TARGET_COMMIT` for `COMMIT_ID`
- when `TARGET_COMMIT` is not set:
  - preserve current behavior

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python3 -m unittest tests/python/test_release_verify.py -v`
Expected: PASS

- [ ] **Step 5: Smoke-check the x86 packager interface**

Run: `TARGET_COMMIT=$(git rev-parse --short HEAD) bash -n scripts/build_pkg_x86_compile.sh`
Expected: exit 0

- [ ] **Step 6: Commit**

```bash
git add scripts/build_pkg_x86_compile.sh scripts/release_verify_lib.py tests/python/test_release_verify.py
git commit -m "Thread target commit through x86 packager"
```

## Chunk 2: ARM Worker And Example Worker

### Task 3: Add the remote ARM package worker

**Files:**
- Create: `scripts/build_pkg_arm_remote.sh`
- Test: `tests/python/test_release_verify.py`

- [ ] **Step 1: Write the failing tests for ARM worker command rendering**

```python
    def test_render_arm_remote_paths_include_commit(self):
        module = load_module()
        cmd = module.render_arm_remote_build(
            commit="abc123",
            vm_host="192.168.3.9",
            vm_port=2222,
            vm_user="axon",
        )
        self.assertIn("abc123", cmd["remote_pkg_name"])
        self.assertIn("192.168.3.9", cmd["ssh_target"])
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python3 -m unittest tests/python/test_release_verify.py -v`
Expected: FAIL with missing ARM render helper.

- [ ] **Step 3: Implement the minimal ARM worker**

Requirements:
- accept `--commit`, `--vm1`, `--vm-user`, `--vm-pass`, `--out-dir`
- copy `git archive <commit>` and `ucx-v1.20.0.tar.gz` to VM1
- build package on VM1 with explicit `COMMIT_ID=<commit>`
- leave unpackable tarball on VM1
- optionally copy tarball to local `out/packages/`
- write build logs to `out/release-verify/<commit>/arm/build.log`
- verify package integrity on VM1 after build:
  - `COMMIT_ID`
  - `ARCH`
  - `README.md`
  - `bin/alps_kv_bench`
  - `bin/ucx_info`
  - `lib/libalps_kv_wrap.so`
  - `lib/libzerokv.so`
  - `lib/ucx/` provider modules
  - compiler/toolchain fingerprint
  - no unexpected `GLIBCXX_*` regression
- record those checks in `out/release-verify/<commit>/arm/package.txt`

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python3 -m unittest tests/python/test_release_verify.py -v`
Expected: PASS

- [ ] **Step 5: Run shell syntax verification**

Run: `bash -n scripts/build_pkg_arm_remote.sh`
Expected: exit 0

- [ ] **Step 6: Commit**

```bash
git add scripts/build_pkg_arm_remote.sh scripts/release_verify_lib.py tests/python/test_release_verify.py
git commit -m "Add remote ARM package worker"
```

### Task 4: Add the example regression worker

**Files:**
- Create: `scripts/release_verify_examples.sh`
- Test: `tests/python/test_release_verify.py`

- [ ] **Step 1: Write the failing tests for example command rendering**

```python
    def test_render_example_matrix_contains_expected_examples(self):
        module = load_module()
        commands = module.render_example_commands(commit="abc123")
        self.assertEqual(
            [item["name"] for item in commands],
            ["ping_pong", "rdma_put_get", "kv_demo", "kv_wait_fetch", "message_kv_demo", "alps_kv_bench"],
        )

    def test_example_sources_distinguish_packaged_and_build_tree_paths(self):
        module = load_module()
        commands = module.render_example_commands(commit="abc123")
        sources = {item["name"]: item["source"] for item in commands}
        self.assertEqual(sources["alps_kv_bench"], "package")
        self.assertEqual(sources["ping_pong"], "build_tree")
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python3 -m unittest tests/python/test_release_verify.py -v`
Expected: FAIL with missing command renderer.

- [ ] **Step 3: Implement the minimal example worker**

Requirements:
- stage the exact commit onto VM1 and VM2 throwaway build trees
- build only requested example targets
- run `alps_kv_bench` from the packaged artifact path
- run `ping_pong`, `rdma_put_get`, `kv_demo`, `kv_wait_fetch`, and `message_kv_demo` from the throwaway build trees
- apply the shared Soft-RoCE env:
  - `UCX_PROTO_ENABLE=n`
  - `UCX_NET_DEVICES=rxe0:1`
  - `UCX_TLS=rc,sm,self`
- log each example under `out/release-verify/<commit>/arm/examples/<name>/`
- stop on first failing example

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python3 -m unittest tests/python/test_release_verify.py -v`
Expected: PASS

- [ ] **Step 5: Run shell syntax verification**

Run: `bash -n scripts/release_verify_examples.sh`
Expected: exit 0

- [ ] **Step 6: Commit**

```bash
git add scripts/release_verify_examples.sh scripts/release_verify_lib.py tests/python/test_release_verify.py
git commit -m "Add release example regression worker"
```

## Chunk 3: Orchestrator, Docs, And Real Verification

### Task 5: Add the top-level orchestrator

**Files:**
- Create: `scripts/release_verify.sh`
- Modify: `README.md`
- Test: `tests/python/test_release_verify.py`

- [ ] **Step 1: Write the failing tests for top-level step ordering and summary output**

```python
    def test_default_step_order_includes_build_smoke_examples_and_perf(self):
        module = load_module()
        plan = module.default_step_plan(skip_x86=False, skip_arm=False,
                                        skip_examples=False, skip_perf=False)
        self.assertEqual(
            plan,
            [
                "x86_package",
                "x86_runtime_smoke",
                "arm_package",
                "arm_softroce_ready",
                "arm_runtime_smoke",
                "arm_alps_e2e",
                "arm_examples",
                "arm_perf",
            ],
        )

    def test_default_step_order_keeps_ready_check_before_e2e(self):
        module = load_module()
        plan = module.default_step_plan(skip_x86=False, skip_arm=False,
                                        skip_examples=False, skip_perf=False)
        self.assertLess(plan.index("arm_softroce_ready"), plan.index("arm_alps_e2e"))

    def test_failure_stops_later_steps_but_keeps_recorded_summary(self):
        module = load_module()
        summary = module.new_summary("abc123")
        should_continue = module.record_step_result(
            summary,
            name="arm_alps_e2e",
            status="fail",
            duration_ms=50,
            log_path="out/release-verify/abc123/arm/e2e/client.log",
            reason="timeout",
        )
        self.assertFalse(should_continue)
        self.assertEqual(summary["steps"][0]["status"], "fail")
        self.assertEqual(summary["steps"][0]["log_path"],
                         "out/release-verify/abc123/arm/e2e/client.log")

    def test_release_perf_command_uses_constrained_softroce_matrix(self):
        module = load_module()
        cmd = module.render_release_perf_command(commit="abc123")
        self.assertIn("run-alps-matrix", cmd)
        self.assertIn("--sizes 1K,1M,32M", cmd)
        self.assertIn("--proto-modes n", cmd)
        self.assertIn("--rma-rails 1", cmd)
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python3 -m unittest tests/python/test_release_verify.py -v`
Expected: FAIL with missing step planner.

- [ ] **Step 3: Implement the orchestrator**

Requirements:
- parse:
  - `--commit`
  - `--skip-x86`
  - `--skip-arm`
  - `--skip-perf`
  - `--skip-examples`
  - `--vm1`
  - `--vm2`
  - `--vm-user`
  - `--vm-pass`
- resolve one pinned commit
- create `out/release-verify/<commit>/`
- invoke:
  - `scripts/build_pkg_x86_compile.sh`
  - `scripts/build_pkg_arm_remote.sh`
  - VM1/VM2 Soft-RoCE readiness checks:
    - `ibv_devices`
    - `ucx_info -d`
    - `rdma link show`
    - NIC/IP consistency validation for the chosen RDMA addresses
  - packaged smoke commands
  - packaged `alps_kv_bench` E2E:
    - VM1 server
    - VM2 client
    - shared Soft-RoCE UCX env
  - `scripts/release_verify_examples.sh`
  - `scripts/perf_experiments.py`
- invoke the perf spot-check with the exact release matrix:
  - subcommand: `run-alps-matrix`
  - `--sizes 1K,1M,32M`
  - `--proto-modes n`
  - `--rma-rails 1`
  - `--extra-env UCX_TLS=rc,sm,self`
  - `--extra-env UCX_NET_DEVICES=rxe0:1`
- write:
  - `summary.txt`
  - `summary.json`
- stop on first failure while preserving logs
- emit explicit step records for:
  - `arm_softroce_ready`
  - `arm_alps_e2e`

- [ ] **Step 4: Run the unit tests to verify they pass**

Run: `python3 -m unittest tests/python/test_release_verify.py -v`
Expected: PASS

- [ ] **Step 5: Run shell syntax verification**

Run: `bash -n scripts/release_verify.sh`
Expected: exit 0

- [ ] **Step 6: Add one short README usage note**

Add a minimal example:

```bash
./scripts/release_verify.sh --commit $(git rev-parse HEAD)
```

- [ ] **Step 7: Commit**

```bash
git add scripts/release_verify.sh scripts/release_verify_lib.py tests/python/test_release_verify.py README.md
git commit -m "Add release verification orchestrator"
```

### Task 6: Run real verification and close the loop

**Files:**
- Verify: `out/release-verify/<commit>/...`

- [ ] **Step 1: Run the focused automated tests**

Run: `python3 -m unittest tests/python/test_release_verify.py -v`
Expected: PASS

- [ ] **Step 2: Run the x86 script syntax checks**

Run: `bash -n scripts/build_pkg_x86_compile.sh scripts/build_pkg_arm_remote.sh scripts/release_verify_examples.sh scripts/release_verify.sh`
Expected: exit 0

- [ ] **Step 3: Run one real release verification pass**

Run: `./scripts/release_verify.sh --commit $(git rev-parse HEAD)`
Expected:
- `out/release-verify/<commit>/summary.txt` exists
- `out/release-verify/<commit>/summary.json` exists
- both package artifact paths are recorded

- [ ] **Step 4: Inspect the final summary**

Run: `cat out/release-verify/$(git rev-parse HEAD)/summary.txt`
Expected: all required steps report `PASS`, or the first failing step points to a concrete log path.

- [ ] **Step 5: Commit**

```bash
git add \
  scripts/build_pkg_x86_compile.sh \
  scripts/build_pkg_arm_remote.sh \
  scripts/release_verify_examples.sh \
  scripts/release_verify.sh \
  scripts/release_verify_lib.py \
  tests/python/test_release_verify.py \
  README.md
git commit -m "Verify release automation flow"
```

## Notes For The Implementer

- Keep the shell entrypoints thin. Put deterministic logic in `scripts/release_verify_lib.py` so tests do not need real VMs.
- Do not silently fall back from a dirty tree to `HEAD`. Release evidence must bind to one commit.
- Reuse `scripts/perf_experiments.py` rather than cloning its matrix logic.
- Do not expand the release package contents in this change just to satisfy example regression; the spec explicitly chooses build-tree validation for non-installed examples.
- Keep logs simple and grep-friendly. Human-readable flat text is preferred over ornate formatting.
