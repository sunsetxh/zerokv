# Implementation Plan: ZeroKV 客户端-服务端通信协议实现

**Branch**: `005-impl-comm-protocol` | **Date**: 2026-03-04 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/005-impl-comm-protocol/spec.md`

## Summary

Implement the client-server communication protocol to enable ZeroKV clients to connect to servers and perform put/get/delete operations. This will use UCX as the transport layer, implementing request-response messaging over UCX connections.

## Technical Context

**Language/Version**: C++17, Python 3.8+
**Primary Dependencies**: UCX 1.19.1, pybind11 (Python bindings)
**Storage**: N/A - distributed KV storage
**Testing**: pytest (Python), GoogleTest (C++), integration tests
**Target Platform**: Linux
**Project Type**: Distributed KV storage library - network protocol
**Performance Goals**: <10ms latency for single operations, <100ms for batch of 100
**Constraints**: Must support RDMA for high-performance AI training scenarios
**Scale/Scope**: 4 user stories, 9 functional requirements

## Constitution Check

*No constitution exists - skipping gates*

## Project Structure

### Documentation (this feature)

```text
specs/005-impl-comm-protocol/
├── plan.md              # This file
├── spec.md              # Feature specification
├── research.md          # Phase 0 output (not needed - UCX already integrated)
├── data-model.md       # Protocol message formats
├── quickstart.md        # Testing guide
├── contracts/           # API contracts
└── tasks.md            # Task list
```

### Source Code (repository root)

```text
src/
├── protocol/
│   ├── message.h       # Message format definitions
│   ├── codec.h         # Encoding/decoding
│   └── handler.h      # Request handlers
├── transport/
│   ├── ucx_transport.h
│   └── ucx_transport.cc
├── server/
│   ├── server.h
│   └── server.cc
└── client/
    ├── client.h
    └── client.cc

python/
├── pybind.cc          # Python bindings
└── tests/
    └── test_zerokv.py

tests/
└── integration/
    ├── fixtures.py
    └── test_cluster.cc
```

**Structure Decision**: Protocol implementation will be in src/protocol/, extending existing transport layer.

## Phase 0: Research

Not needed - UCX is already integrated in the project. The main work is implementing the actual communication protocol on top of UCX.

## Phase 1: Design

### Protocol Message Format (data-model.md)

Define binary message format for client-server communication:

```
Request Message:
| op_code (1B) | flags (1B) | key_len (4B) | value_len (4B) | key (N bytes) | value (M bytes) |

Response Message:
| status (1B) | flags (1B) | value_len (4B) | value (N bytes) |
```

Operation Codes:
- 0x01: PUT
- 0x02: GET
- 0x03: DELETE
- 0x04: BATCH_PUT
- 0x05: BATCH_GET

Status Codes:
- 0x00: OK
- 0x01: NOT_FOUND
- 0x02: ERROR

### Interface Contracts

Server side:
- UCX listener accepts connections
- Receive request messages
- Process using Storage engine
- Send response messages

Client side:
- Connect to server endpoint
- Send request messages
- Receive and decode responses

## Notes

- Protocol implementation requires coordination between client and server
- Need to ensure server can handle concurrent connections
- Integration tests will verify end-to-end functionality
