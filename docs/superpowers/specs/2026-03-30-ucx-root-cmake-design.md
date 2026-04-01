# UCX Root CMake Option Design

**Date:** 2026-03-30

## Goal

Allow ZeroKV to locate a non-system UCX installation through a single explicit
CMake option:

```bash
-DUCX_ROOT=/path/to/ucx
```

## Scope

In scope:

- add a `UCX_ROOT` CMake cache path option
- use `UCX_ROOT` to extend `PKG_CONFIG_PATH`
- keep the existing `pkg-config`-based UCX discovery flow
- support common install layouts under `lib/pkgconfig` and `lib64/pkgconfig`

Out of scope:

- replacing `pkg-config` with manual library discovery
- adding `UCX_INCLUDE_DIR` / `UCX_LIBRARY_DIR` style knobs
- supporting multiple UCX roots
- runtime linker changes such as `LD_LIBRARY_PATH`

## Design

Add this cache variable near the existing build options:

```cmake
set(UCX_ROOT "" CACHE PATH "Root directory of a UCX installation")
```

If `UCX_ROOT` is set:

- build a list of candidate pkg-config directories:
  - `${UCX_ROOT}/lib/pkgconfig`
  - `${UCX_ROOT}/lib64/pkgconfig`
- append only existing directories to `ENV{PKG_CONFIG_PATH}`
- preserve any existing `PKG_CONFIG_PATH`
- print a status message showing the effective UCX pkg-config search dirs

Then continue using the existing discovery path:

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(UCX REQUIRED IMPORTED_TARGET ucx)
```

This keeps the rest of the build unchanged:

- `PkgConfig::UCX` remains the link target
- include paths and link flags still come from the UCX `.pc` file

## Behavior

Expected usage:

```bash
cmake -S . -B build -DUCX_ROOT=/opt/ucx
```

Expected result:

- if `/opt/ucx/lib/pkgconfig/ucx.pc` exists, ZeroKV finds UCX there
- if `/opt/ucx/lib64/pkgconfig/ucx.pc` exists instead, ZeroKV finds it there
- if neither exists, configuration still fails in the normal `pkg_check_modules`
  path with a clear missing-package error

## Error Handling

- `UCX_ROOT` is optional
- non-existent pkg-config subdirectories are ignored silently
- if `UCX_ROOT` is set but no valid UCX pkg-config directory is found, ZeroKV
  still fails through the normal `pkg-config` check
- no fallback manual probing is added in this phase

## Verification

At minimum:

1. configure without `UCX_ROOT` still behaves exactly as before
2. configure with `-DUCX_ROOT=<valid path>` succeeds when that UCX install has
   a working `ucx.pc`
3. configure with `-DUCX_ROOT=<path without ucx.pc>` fails cleanly

## Non-Goals

This change is only about build-time discovery.

It does not guarantee runtime loading of the same UCX unless the runtime linker
environment is also configured correctly, for example through:

- `ldconfig`
- `LD_LIBRARY_PATH`
- RPATH settings outside this change
