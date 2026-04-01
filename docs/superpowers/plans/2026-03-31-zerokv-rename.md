# ZeroKV Rename Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename the repository from `axon` to `zerokv` in one non-compatible cut across build, C++, Python, scripts, docs, and packaging while preserving behavior.

**Architecture:** This is a semantic-only rename. The work proceeds in small verified batches: first rename build/package/include roots atomically, then rename C++ namespaces and API references, then Python and plugin surfaces, then env vars/scripts/docs, and finally run repo-wide grep and build/runtime verification. No compatibility shims or forwarding aliases are introduced.

**Tech Stack:** CMake, C++17, UCX, nanobind, Python 3, GoogleTest, shell scripts

---

## File Map

**Core build / packaging**
- Modify: `CMakeLists.txt` — project name, target names, install/export package, Python output dir, option variable names
- Modify: `scripts/package_source.sh` — source tarball prefix `zerokv-src-*`
- Modify: `README.md` — build/install/import/package commands
- Modify: `CLAUDE.md`
- Modify: `GEMINI.md`

**Public C++ headers**
- Create via move: `include/zerokv/zerokv.h`
- Create via move: `include/zerokv/cluster.h`
- Create via move: `include/zerokv/common.h`
- Create via move: `include/zerokv/config.h`
- Create via move: `include/zerokv/endpoint.h`
- Create via move: `include/zerokv/future.h`
- Create via move: `include/zerokv/kv.h`
- Create via move: `include/zerokv/memory.h`
- Create via move: `include/zerokv/worker.h`
- Create via move: `include/zerokv/plugin/plugin.h`
- Delete after move: `include/axon/...`

**C++ implementation / tests / examples**
- Modify: `src/*.cpp`, `src/internal/*.h`, `src/kv/*`, `src/plugin/*.cpp`, `src/python/bindings.cpp`
- Modify: `examples/*.cpp`
- Modify: `tests/unit/*.cpp`
- Modify: `tests/integration/*.cpp`
- Modify: `tests/benchmark/*.cpp`

**Python**
- Create via move: `python/zerokv/__init__.py`
- Create via move: `python/zerokv/_core.pyi`
- Delete after move: `python/axon/*`
- Modify: `python/examples/kv_example.py`
- Modify: `examples/python_usage.py`
- Modify: `tests/python/test_python_api_surface.py`

**Docs / reports / active specs/plans**
- Modify: `docs/INDEX.md`
- Modify: `docs/reports/zerokv-rdma-kv-mvp.md`
- Modify active docs/specs/plans that still contain actionable build or API references
- Leave purely historical archived reports untouched unless they are current user guidance

### Task 1: Atomic Build and Include Root Rename

**Files:**
- Modify: `CMakeLists.txt`
- Move/Create: `include/zerokv/*.h`
- Delete: `include/axon/*.h`

- [ ] **Step 1: Write the failing file/path assertions**

Add a temporary shell-based verification note for this batch:

```bash
find include -maxdepth 2 -type f | sort
rg -n '#include "axon/' include src examples tests
```

Expected before implementation:
- `include/axon/*.h` exists
- grep finds old include paths

- [ ] **Step 2: Rename the public include tree and umbrella header**

Move headers so the public tree becomes:

```text
include/zerokv/zerokv.h
include/zerokv/cluster.h
include/zerokv/common.h
include/zerokv/config.h
include/zerokv/endpoint.h
include/zerokv/future.h
include/zerokv/kv.h
include/zerokv/memory.h
include/zerokv/worker.h
include/zerokv/plugin/plugin.h
```

Delete the old `include/axon` tree after references are updated in the same task.

- [ ] **Step 3: Update CMake project/package names atomically with include install path**

Edit `CMakeLists.txt` so these identifiers change together:

```cmake
project(zerokv VERSION 1.0.0 LANGUAGES CXX)

option(ZEROKV_BUILD_STATIC     "Build static library"                      OFF)
option(ZEROKV_BUILD_EXAMPLES   "Build examples"                           ON)
option(ZEROKV_BUILD_TESTS      "Build tests (requires GTest)"             ON)
option(ZEROKV_BUILD_BENCHMARK  "Build benchmarks (requires Google Benchmark)" ON)
option(ZEROKV_BUILD_PYTHON     "Build Python bindings (requires nanobind)" ON)
set(ZEROKV_THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party" CACHE PATH "Root directory for vendored third-party dependencies")

add_library(zerokv SHARED ${ZEROKV_SOURCES})
add_library(zerokv_static STATIC ${ZEROKV_SOURCES})
set_target_properties(zerokv_static PROPERTIES OUTPUT_NAME zerokv)

install(TARGETS zerokv EXPORT zerokvTargets ...)
install(DIRECTORY include/zerokv DESTINATION include)
install(EXPORT zerokvTargets
    NAMESPACE zerokv::
    DESTINATION lib/cmake/zerokv)
```

Also update all conditionals and target links to use `ZEROKV_*` variables and `zerokv`/`zerokv_static` target names.

- [ ] **Step 4: Update all source includes to the new root**

Apply the mechanical include rewrite across active code:

```cpp
#include "zerokv/config.h"
#include "zerokv/endpoint.h"
#include "zerokv/kv.h"
```

This batch must leave zero active `#include "axon/..."` references in code.

- [ ] **Step 5: Run build-root verification**

Run:

```bash
rg -n '#include "axon/' include src examples tests
find include -maxdepth 2 -type f | sort
cmake -S . -B build -DZEROKV_BUILD_PYTHON=OFF -DZEROKV_BUILD_TESTS=OFF -DZEROKV_BUILD_BENCHMARK=OFF
```

Expected:
- grep returns no matches in active code
- `include/zerokv/...` exists
- CMake configure succeeds

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt include src examples tests
 git commit -m "Rename build and public include roots to zerokv"
```

### Task 2: Rename C++ Namespaces and Error/Plugin Symbols

**Files:**
- Modify: `include/zerokv/*.h`
- Modify: `src/*.cpp`
- Modify: `src/internal/*.h`
- Modify: `src/kv/*`
- Modify: `src/plugin/*.cpp`
- Modify: `tests/unit/*.cpp`
- Modify: `tests/integration/*.cpp`
- Modify: `tests/benchmark/*.cpp`
- Modify: `examples/*.cpp`

- [ ] **Step 1: Write the failing grep checks**

Run:

```bash
rg -n '\bnamespace axon\b|\baxon::|AXON_PLUGIN_EXPORT|axon_plugin_create|axon_category\(' include src tests examples
```

Expected before implementation:
- matches across headers, sources, tests, examples

- [ ] **Step 2: Rename namespaces and API qualifiers**

Mechanical rewrite examples:

```cpp
namespace zerokv {
...
} // namespace zerokv

namespace zerokv::kv {
...
} // namespace zerokv::kv

using zerokv::kv::KVNode;
auto cfg = zerokv::Config::builder().set_transport("tcp").build();
```

Update all `using`, `namespace proto = ...`, explicit qualifiers, comments, and doc snippets inside active code/tests/examples.

- [ ] **Step 3: Rename error category and plugin symbols**

In `include/zerokv/common.h`, `src/status.cpp`, and plugin headers/sources, rename:

```cpp
const std::error_category& zerokv_category() noexcept;

#define ZEROKV_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
ZEROKV_PLUGIN_EXPORT zerokv::plugin::CollectivePlugin* zerokv_plugin_create();
```

Also update any `name()` string payload from `"axon"` to `"zerokv"`.

- [ ] **Step 4: Rename plugin shared-object naming convention in comments/docs/code**

Update plugin-facing references such as:

```text
libaxon_plugin_nccl.so -> libzerokv_plugin_nccl.so
```

and discovery comments/search text in `include/zerokv/plugin/plugin.h` and plugin sources.

- [ ] **Step 5: Run namespace/symbol verification**

Run:

```bash
rg -n '\bnamespace axon\b|\baxon::|AXON_PLUGIN_EXPORT|axon_plugin_create|axon_category\(' include src tests examples
cmake --build build -j4
```

Expected:
- grep returns no active-code matches
- build succeeds

- [ ] **Step 6: Commit**

```bash
git add include src tests examples
 git commit -m "Rename C++ namespaces and plugin symbols to zerokv"
```

### Task 3: Rename Python Package and Bindings Output

**Files:**
- Move/Create: `python/zerokv/__init__.py`
- Move/Create: `python/zerokv/_core.pyi`
- Delete: `python/axon/*`
- Modify: `src/python/bindings.cpp`
- Modify: `python/examples/kv_example.py`
- Modify: `examples/python_usage.py`
- Modify: `tests/python/test_python_api_surface.py`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing Python-path checks**

Run:

```bash
find python -maxdepth 2 -type f | sort
rg -n '\bimport axon\b|python/axon|axon\._core' python examples tests CMakeLists.txt
```

Expected before implementation:
- `python/axon/*` exists
- grep finds old imports/output path

- [ ] **Step 2: Move the Python package directory**

Move:

```text
python/axon/__init__.py -> python/zerokv/__init__.py
python/axon/_core.pyi -> python/zerokv/_core.pyi
```

Update package exports to expose `zerokv` instead of `axon`.

- [ ] **Step 3: Update binding output directory and imports**

In `CMakeLists.txt`, change:

```cmake
LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/python/zerokv"
```

Update all Python/example/test imports:

```python
import zerokv
server = zerokv.KVServer(...)
```

- [ ] **Step 4: Run Python rename verification**

Run:

```bash
rg -n '\bimport axon\b|python/axon|axon\._core' python examples tests CMakeLists.txt
python3 -m py_compile python/zerokv/__init__.py python/examples/kv_example.py examples/python_usage.py
```

Expected:
- grep returns no active matches
- py_compile succeeds

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt python examples tests src/python/bindings.cpp
 git commit -m "Rename Python package to zerokv"
```

### Task 4: Rename Environment Variables, CMake Flags, and Scripts

**Files:**
- Modify: `src/config.cpp`
- Modify: `README.md`
- Modify: `scripts/package_source.sh`
- Modify: `scripts/qemu_rdma/*.sh`
- Modify: `scripts/rdma_sim/entrypoint.sh`
- Modify active docs/specs/plans containing current build/run instructions
- Modify: `tests/unit/test_config.cpp`

- [ ] **Step 1: Write the failing config grep checks**

Run:

```bash
rg -n 'ZEROKV_BUILD_|ZEROKV_TRANSPORT|ZEROKV_NUM_WORKERS|ZEROKV_MEM_POOL_SIZE|ZEROKV_THIRD_PARTY_DIR|getenv\("ZEROKV_' CMakeLists.txt src README.md scripts tests docs --glob '!docs/reports/report-*' --glob '!docs/meetings/**'
```

Expected before implementation:
- matches in config, build docs, scripts, tests

- [ ] **Step 2: Rename runtime env vars in code and tests**

Update `src/config.cpp` and tests to read only:

```cpp
std::getenv("ZEROKV_TRANSPORT")
std::getenv("ZEROKV_NUM_WORKERS")
std::getenv("ZEROKV_MEM_POOL_SIZE")
```

No fallback to `AXON_*`.

- [ ] **Step 3: Rename CMake options and script usage**

Update command examples and script invocations to use:

```bash
-DZEROKV_BUILD_STATIC=ON
-DZEROKV_BUILD_TESTS=ON
-DZEROKV_BUILD_BENCHMARK=ON
-DZEROKV_BUILD_PYTHON=OFF
```

Update packaging script output prefix to:

```bash
zerokv-src-$(date +%Y%m%d)-${short_sha}.tar.gz
```

- [ ] **Step 4: Run env/script verification**

Run:

```bash
rg -n 'ZEROKV_BUILD_|ZEROKV_TRANSPORT|ZEROKV_NUM_WORKERS|ZEROKV_MEM_POOL_SIZE|ZEROKV_THIRD_PARTY_DIR|getenv\("ZEROKV_' CMakeLists.txt src README.md scripts tests docs --glob '!docs/reports/report-*' --glob '!docs/meetings/**'
./scripts/package_source.sh
ls -1 zerokv-src-*.tar.gz | tail -n 1
```

Expected:
- grep returns no active matches
- package script emits `zerokv-src-*`

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/config.cpp scripts README.md tests docs
 git commit -m "Rename config knobs and packaging to zerokv"
```

### Task 5: Rename Active Docs, Examples, and User-Facing References

**Files:**
- Modify: `README.md`
- Modify: `CLAUDE.md`
- Modify: `GEMINI.md`
- Modify: `docs/INDEX.md`
- Modify: `docs/reports/zerokv-rdma-kv-mvp.md`
- Modify active `docs/superpowers/specs/*.md`
- Modify active `docs/superpowers/plans/*.md`
- Modify examples/tests with user-facing output strings

- [ ] **Step 1: Write the failing active-doc grep checks**

Run:

```bash
rg -n '\baxon\b|AXON_' README.md CLAUDE.md GEMINI.md examples tests docs/superpowers docs/INDEX.md docs/reports/zerokv-rdma-kv-mvp.md --glob '!**/report-*' --glob '!**/meeting-*'
```

Expected before implementation:
- matches in active docs and user-facing examples

- [ ] **Step 2: Rewrite active docs/examples to ZeroKV / zerokv**

Apply these rules:

```text
Project/product prose -> ZeroKV
Package/namespace/path/code examples -> zerokv
```

Do not spend time rewriting archived historical reports beyond current user guidance.

- [ ] **Step 3: Run doc/output verification**

Run:

```bash
rg -n '\baxon\b|AXON_' README.md CLAUDE.md GEMINI.md examples tests docs/superpowers docs/INDEX.md docs/reports/zerokv-rdma-kv-mvp.md --glob '!**/report-*' --glob '!**/meeting-*'
```

Expected:
- no active guidance still tells users to use `axon` or `AXON_*`

- [ ] **Step 4: Commit**

```bash
git add README.md CLAUDE.md GEMINI.md examples tests docs
 git commit -m "Rename active docs and examples to ZeroKV"
```

### Task 6: Full Verification and Installation Smoke

**Files:**
- Verify only

- [ ] **Step 1: Run repo-wide residual-name checks**

Run:

```bash
rg -n '\bnamespace axon\b|\baxon::|#include "axon/|\bimport axon\b|AXON_' . --glob '!third_party/**' --glob '!build/**' --glob '!docs/reports/report-*' --glob '!docs/meetings/**'
find . -path ./third_party -prune -o -path ./build -prune -o -name '*axon*' -print
```

Expected:
- no active-code/style matches
- `find` returns only intentionally historical/archive artifacts if any remain; otherwise empty

- [ ] **Step 2: Configure and build representative targets**

Run:

```bash
rm -rf build
cmake -S . -B build \
  -DZEROKV_BUILD_TESTS=ON \
  -DZEROKV_BUILD_BENCHMARK=ON \
  -DZEROKV_BUILD_PYTHON=OFF
cmake --build build -j4 --target zerokv kv_demo kv_bench test_kv_server test_kv_node test_kv_bench_integration
```

Expected:
- configure succeeds under `ZEROKV_*`
- representative targets build successfully

- [ ] **Step 3: Run representative tests**

Run:

```bash
ctest --test-dir build -R 'IntegrationKvServer|IntegrationKvNode|IntegrationKvBench' --output-on-failure
```

Expected:
- all selected tests pass

- [ ] **Step 4: Run install/package/import smoke**

Run:

```bash
cmake --install build --prefix /tmp/zerokv-install
find /tmp/zerokv-install -maxdepth 4 | sort | sed -n '1,120p'
```

Expected:
- install tree contains `include/zerokv`
- install tree contains `lib/cmake/zerokv`
- installed library filename is `libzerokv.*`

- [ ] **Step 5: Run downstream CMake smoke with find_package(zerokv)**

Create `/tmp/zerokv-smoke/CMakeLists.txt` with:

```cmake
cmake_minimum_required(VERSION 3.16)
project(zerokv_smoke LANGUAGES CXX)
find_package(zerokv REQUIRED CONFIG PATHS /tmp/zerokv-install/lib/cmake/zerokv NO_DEFAULT_PATH)
add_executable(smoke main.cpp)
target_link_libraries(smoke PRIVATE zerokv::zerokv)
```

Create `/tmp/zerokv-smoke/main.cpp` with:

```cpp
#include <zerokv/kv.h>
int main() {
    auto cfg = zerokv::Config::builder().set_transport("tcp").build();
    (void)cfg;
    return 0;
}
```

Run:

```bash
cmake -S /tmp/zerokv-smoke -B /tmp/zerokv-smoke/build
cmake --build /tmp/zerokv-smoke/build -j4
```

Expected:
- `find_package(zerokv)` succeeds
- downstream build succeeds against `<zerokv/kv.h>` and `zerokv::zerokv`

- [ ] **Step 6: Optional Python smoke when enabled**

If Python is enabled in the environment, run:

```bash
cmake -S . -B build-py -DZEROKV_BUILD_PYTHON=ON -DZEROKV_BUILD_TESTS=OFF -DZEROKV_BUILD_BENCHMARK=OFF
cmake --build build-py -j4 --target _core
PYTHONPATH=/Users/wangyuchao/code/axon/python python3 - <<'PY'
import importlib
import zerokv
print(zerokv.__name__)
try:
    importlib.import_module('axon')
    raise SystemExit('import axon unexpectedly succeeded')
except ModuleNotFoundError:
    pass
PY
```

Expected:
- `import zerokv` succeeds
- `import axon` fails with `ModuleNotFoundError`

- [ ] **Step 7: Commit final verification-only follow-ups if needed**

```bash
git status --short
```

Expected:
- clean worktree, or only intentional follow-up edits before final commit
