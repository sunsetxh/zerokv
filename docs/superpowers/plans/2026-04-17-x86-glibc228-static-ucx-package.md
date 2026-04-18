# X86 GLIBC 2.28 Static UCX Package Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce an `x86_64` ALPS compatibility package built against `glibc 2.28` with UCX linked statically into the shipped ZeroKV shared libraries.

**Architecture:** Reuse the existing ALPS package layout, but build inside a reproducible `glibc 2.28` container instead of the current `openEuler 2.34` compile container. The packaging script will bootstrap static PIC UCX inside that container, build/install the required targets, archive the package, and verify the resulting ELF dependencies and GLIBC symbol floor.

**Tech Stack:** Bash, Docker, Rocky Linux 8 / glibc 2.28, GCC toolset, CMake, UCX 1.20.0, objdump/readelf/ldd

---

### Task 1: Inspect and lock the packaging baseline

**Files:**
- Modify: `scripts/build_pkg_x86_compile.sh`
- Check: `CMakeLists.txt`
- Check: `docs/alps_kv_wrap/README.md`

- [ ] **Step 1: Confirm the current x86 packaging script does not pin glibc 2.28**

Run: `sed -n '1,260p' scripts/build_pkg_x86_compile.sh`
Expected: script uses an external `compile` container but does not assert `glibc 2.28`.

- [ ] **Step 2: Confirm the current compile container is unsuitable**

Run: `docker exec compile bash -lc 'cat /etc/os-release; ldd --version | sed -n "1,2p"'`
Expected: current container reports a glibc newer than 2.28.

- [ ] **Step 3: Confirm CMake already supports static UCX linkage**

Run: `rg -n "ZEROKV_LINK_UCX_STATIC|pkg-config --static" CMakeLists.txt`
Expected: static UCX link path already exists and should be reused.

### Task 2: Add a deterministic glibc 2.28 packaging path

**Files:**
- Modify: `scripts/build_pkg_x86_compile.sh`

- [ ] **Step 1: Replace the remote-host/container dependency with a local Docker flow that can launch a glibc 2.28 image**

Implementation:
- allow overriding image/container names via env vars
- default to a Rocky Linux 8 style image (`glibc 2.28`)
- create and remove a temporary build container inside the script

- [ ] **Step 2: Install build prerequisites inside the container**

Implementation:
- install compiler, CMake, make, pkg-config, perl/python/autotools, rdma-core/libnl/numactl development packages needed by UCX and ZeroKV
- keep the runtime base on glibc 2.28

- [ ] **Step 3: Build static PIC UCX inside the container**

Implementation:
- unpack `ucx-v1.20.0.tar.gz`
- configure with `-fPIC`
- install to `/opt/ucx-1.20.0-static-pic`

- [ ] **Step 4: Build and install the package targets**

Implementation:
- archive current `HEAD`
- configure with `-DUCX_ROOT=/opt/ucx-1.20.0-static-pic -DZEROKV_LINK_UCX_STATIC=ON`
- build `zerokv`, `alps_kv_wrap`, `alps_kv_bench`
- install to a packaged directory and add `README.md`, `COMMIT_ID`, `ARCH`, `ucx_info`, `ucp_info`

### Task 3: Add packaging verification for the requested ABI contract

**Files:**
- Modify: `scripts/build_pkg_x86_compile.sh`

- [ ] **Step 1: Verify container glibc version before build**

Implementation:
- fail fast unless container reports `glibc 2.28`

- [ ] **Step 2: Verify produced binaries do not depend on shared UCX libraries**

Implementation:
- use `ldd` or `readelf -d`
- fail if `libucp.so`, `libucs.so`, `libuct.so`, or `libucm.so` appear as needed shared libs

- [ ] **Step 3: Verify produced binaries’ GLIBC symbol requirements stay at or below 2.28**

Implementation:
- inspect `objdump -T` / `readelf --version-info`
- emit the highest `GLIBC_*` symbol version for shipped ELF files
- fail if any file requires above `GLIBC_2.28`

### Task 4: Run packaging and inspect artifacts

**Files:**
- Output: `out/packages/alps_kv_wrap_pkg-x86_64-<commit>.tar.gz`
- Output: `out/packages/alps_kv_wrap_pkg-x86_64.tar.gz`

- [ ] **Step 1: Run the packaging script**

Run: `scripts/build_pkg_x86_compile.sh`
Expected: tarball created under `out/packages/`

- [ ] **Step 2: Inspect the package contents**

Run: `tar -tzf out/packages/alps_kv_wrap_pkg-x86_64-<commit>.tar.gz | sed -n '1,80p'`
Expected: includes `include/yr/alps_kv_api.h`, `lib/libalps_kv_wrap.so`, `lib/libzerokv.so`, `bin/alps_kv_bench`, metadata files

- [ ] **Step 3: Record ABI verification output**

Run: verification commands emitted by the script
Expected: container glibc = 2.28, no dynamic UCX dependency, max GLIBC requirement <= 2.28
