# Implementation Plan: ZeroKV SDK 集成支持

**Branch**: `003-sdk-integration` | **Date**: 2026-03-03 | **Spec**: [spec.md](./spec.md)

## Summary

Enable ZeroKV to be used as an embedded SDK in user applications. Both Server and Client can be linked as libraries and used directly in C++ code without running separate processes.

## Technical Context

**Language/Version**: C++17
**Primary Dependencies**: UCX 1.19.1, CMake 3.15+
**Storage**: In-memory with LRU eviction
**Testing**: gtest, custom test frameworks
**Target Platform**: Linux/macOS/Windows
**Project Type**: Distributed KV storage library with SDK support

## Constitution Check

*No constitution exists - skipping gates*

## Project Structure

### Source Code (repository root)

```
zerokv/
├── include/zerokv/     # Public headers (SDK)
│   ├── client.h        # Client API (existing)
│   ├── server.h        # Server API (existing)
│   ├── storage.h       # Storage API
│   └── ...
├── src/                # Core implementation
├── python/             # Python bindings
├── build/              # CMake build output
└── examples/          # Usage examples
```

**Structure Decision**: Already has proper header-based API. Need to verify CMake integration.

## Phase 0: Research

### Existing Implementation Status

| Component | Status | Notes |
|-----------|--------|-------|
| server.h | ✅ Existing | Has start/stop methods |
| client.h | ✅ Existing | Has connect/put/get methods |
| CMake config | ⚠️ Basic | Needs find_package support |

### What needs work

1. CMake find_package support
2. Static/Dynamic library packaging
3. API documentation
4. Usage examples

## Phase 1: Design

### Data Model

N/A - This is an SDK/API enhancement, no data model changes needed.

---

**Phase 1 Complete** - Ready for `/speckit.tasks` to generate task list.
