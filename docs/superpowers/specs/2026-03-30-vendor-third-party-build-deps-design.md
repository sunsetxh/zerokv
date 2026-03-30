# Vendor Third-Party Build Dependencies Design

**Date:** 2026-03-30

## Goal

Allow AXON to build tests, benchmarks, and Python bindings from vendored local
source trees instead of requiring system-installed `GTest`, `Google Benchmark`,
or `nanobind`.

## Scope

In scope:

- support vendored local source directories under `third_party/`
- prefer vendored source trees over system `find_package(...)`
- cover these dependencies:
  - `googletest`
  - `benchmark`
  - `nanobind`
- preserve current build options:
  - `AXON_BUILD_TESTS`
  - `AXON_BUILD_BENCHMARK`
  - `AXON_BUILD_PYTHON`

Out of scope:

- vendoring UCX
- automatic downloading through `FetchContent`
- rewriting AXON tests or examples
- forcing vendored builds when system packages are preferred

## Directory Convention

Expected vendor layout:

```text
third_party/
  googletest/
  benchmark/
  nanobind/
```

The source package may include these directories directly, or downstream
packaging may populate them before configuring CMake.

## Design

### GoogleTest

When `AXON_BUILD_TESTS=ON`:

1. if `third_party/googletest/CMakeLists.txt` exists:
   - use `add_subdirectory(third_party/googletest ...)`
   - consume targets provided by vendored GoogleTest
2. otherwise:
   - fall back to `find_package(GTest QUIET)`
   - keep the current "tests will not be built" downgrade behavior

### Google Benchmark

When `AXON_BUILD_BENCHMARK=ON`:

1. if `third_party/benchmark/CMakeLists.txt` exists:
   - use `add_subdirectory(third_party/benchmark ...)`
   - consume vendored benchmark targets
2. otherwise:
   - fall back to `find_package(GBenchmark QUIET)`
   - keep the current downgrade behavior

### nanobind

When `AXON_BUILD_PYTHON=ON`:

1. if `third_party/nanobind/CMakeLists.txt` exists:
   - use `add_subdirectory(third_party/nanobind ...)`
   - use vendored nanobind directly
2. otherwise:
   - fall back to the current Python import + `find_package(nanobind CONFIG REQUIRED)` flow
   - if that path fails, keep the current downgrade behavior

## Behavior

The build preference order becomes:

1. vendored source under `third_party/`
2. system package discovery
3. optional feature disabled if dependency still unavailable

This keeps local development flexible while making offline packaging possible.

## Error Handling

- missing vendor directories are not errors by themselves
- vendor path is used only if it contains a `CMakeLists.txt`
- if neither vendor nor system dependency is available:
  - tests are disabled
  - benchmarks are disabled
  - Python bindings are disabled
- no network fetch is introduced

## Packaging Benefit

A source package that contains:

- AXON source
- `third_party/googletest`
- `third_party/benchmark`
- `third_party/nanobind`

can be built offline without requiring separate dev packages for those three
dependencies.

## Verification

At minimum:

1. configure with empty `third_party/` still behaves exactly like today
2. configure with vendored `googletest` and no system GTest still enables tests
3. configure with vendored `benchmark` and no system benchmark still enables benchmarks
4. configure with vendored `nanobind` and no system nanobind still enables Python bindings
5. configure with all vendor dirs present works without network access

## Non-Goals

This change does not require checking third-party source trees into the current
repository immediately.

It only adds the build-system support and directory convention so packaging can
place the vendored source trees there.
