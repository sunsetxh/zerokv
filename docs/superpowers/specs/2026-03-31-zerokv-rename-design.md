# ZeroKV Rename Design

## Goal
Rename the project from `axon` to `zerokv` across the repository in one hard cut, with no compatibility layer. The change is semantic only: no behavior, protocol, or API shape changes beyond identifiers, paths, package names, build target names, and documentation references.

## Naming Rules
- Public/product name in prose: `ZeroKV`
- Code-level canonical name: `zerokv`
- C++ namespace: `zerokv`
- Public include root: `include/zerokv/`
- Python package: `python/zerokv`
- CMake project and exported package: `zerokv`
- Environment variables and CMake options prefix: `ZEROKV_`
- Source package naming: `zerokv-src-YYYYMMDD-<sha>.tar.gz`

## Scope
This rename covers:
- CMake project name, build targets, install/export package names
- Public include paths under `include/`
- All `namespace axon` / `axon::...` references
- Python package/module path and imports
- Environment variables and CMake options prefixed with `AXON_`
- README, docs, scripts, examples, tests, benchmark output labels, package scripts

Out of scope:
- Protocol message names and wire-format compatibility changes
- Functional refactors or behavior changes
- Backward-compatibility aliases, forwarding headers, or deprecated old names

## Detailed Changes

### 1. Build and Packaging
- `project(axon)` -> `project(zerokv)`
- Library targets:
  - `axon` -> `zerokv`
  - `axon_static` -> `zerokv_static`
- Install/export package:
  - `axonTargets` -> `zerokvTargets`
  - `NAMESPACE axon::` -> `NAMESPACE zerokv::`
  - install package directory `lib/cmake/axon` -> `lib/cmake/zerokv`
- Public include install:
  - `include/axon` -> `include/zerokv`
- Python extension output directory:
  - `python/axon` -> `python/zerokv`
- Source package default name:
  - `axon-src-*` -> `zerokv-src-*`

### 2. C++ API Surface
- Move public headers from `include/axon/*.h` to `include/zerokv/*.h`
- Rename umbrella header file `include/axon/axon.h` to `include/zerokv/zerokv.h` and update all call sites accordingly
- Replace all public/private includes:
  - `#include "axon/..."` -> `#include "zerokv/..."`
- Rename top-level namespace:
  - `namespace axon` -> `namespace zerokv`
  - `axon::kv` -> `zerokv::kv`
- Plugin factory names/macros also rename consistently if they are repo-scoped identifiers:
  - `AXON_PLUGIN_EXPORT` -> `ZEROKV_PLUGIN_EXPORT`
  - `axon_plugin_create` -> `zerokv_plugin_create`
  - any plugin shared-object discovery pattern such as `libaxon_plugin_*.so` -> `libzerokv_plugin_*.so`
- Error category and library naming also rename consistently:
  - `axon_category()` -> `zerokv_category()`
  - error-category `name()` string payloads mentioning `axon` -> `zerokv`
  - resulting shared/static library filenames `libaxon.*` -> `libzerokv.*`

### 3. Python
- Rename package directory:
  - `python/axon` -> `python/zerokv`
- Update imports in examples/tests:
  - `import axon` -> `import zerokv`
- Keep extension module file as `_core`, but under `python/zerokv`
- Update generated/stub files accordingly:
  - `python/zerokv/_core.pyi`
  - `python/zerokv/__init__.py`

### 4. Config and Environment
- CMake options:
  - `ZEROKV_BUILD_STATIC`, `ZEROKV_BUILD_EXAMPLES`, `ZEROKV_BUILD_TESTS`,
    `ZEROKV_BUILD_BENCHMARK`, `ZEROKV_BUILD_PYTHON`, and
    `ZEROKV_THIRD_PARTY_DIR`
- Runtime env vars:
  - `ZEROKV_TRANSPORT`, `ZEROKV_NUM_WORKERS`, and `ZEROKV_MEM_POOL_SIZE`
  - any other active `getenv("ZEROKV_...")` site found by repo-wide grep must be covered by the cut
- No fallback support for legacy names

### 5. Docs and Scripts
- Rename prose references from `axon` to `ZeroKV` where referring to the project/product
- Rename code references to `zerokv` where referring to package/namespace/paths
- Update scripts and CI/build snippets to use new CMake options and package paths
- Update README build instructions, Python examples, benchmark examples, source-package examples
- Update active assistant/config docs such as `CLAUDE.md` and `GEMINI.md` if they contain current project-name references or build knobs

## Execution Strategy
Perform the rename in this order to keep the tree buildable after each logical batch:
1. CMake/project/package names plus public include tree move/include updates as one atomic batch
2. Namespace rename across source/tests/examples
3. Python package move and import updates
4. Env vars / options / scripts
5. Docs and packaging script updates
6. Full repo grep to ensure no intentional `axon` references remain outside historical documents or vendored code

## Verification
At minimum, after the rename:
- Configure/build core library and examples
- Build KV tests and benchmark tests
- Run representative tests:
  - KV node/server integration tests
  - benchmark integration tests
  - Python syntax smoke if Python enabled
- Grep/content checks:
  - no `namespace axon`
  - no `#include "axon/`
  - no `import axon`
  - no `AXON_` in active code/docs/scripts except historical archived docs if intentionally preserved
- File/path checks:
  - `find . -path ./third_party -prune -o -path ./build -prune -o -name "*axon*" -print` should not return active source/build/runtime files
- Install/import checks:
  - installed package resolves with `find_package(zerokv)`
  - Python smoke uses `import zerokv` and confirms `import axon` fails
  - plugin loading smoke validates renamed plugin filename/factory conventions still work

## Risks
- Large mechanical diff; easy to miss generated/package/script paths
- Python output path changes can break local build assumptions
- Install/export package rename may break downstream consumers immediately; this is accepted because the cut is intentionally non-compatible
- Historical reports/specs may still mention `axon`; current user-facing docs and active build files must be updated, but archived historical documents may remain untouched if clearly historical

## Success Criteria
- Repository builds and tests under the new `zerokv` name
- Public code examples use `zerokv` only
- Packaging script emits `zerokv-src-*`
- No active build/runtime path still requires the old `axon` name
