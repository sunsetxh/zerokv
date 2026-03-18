"""
axon - High-performance AXON transport library with Python bindings.

Usage:
    import axon

    ctx = axon.Context(transport="ucx", num_workers=2)
    worker = ctx.create_worker()
    ep = worker.connect("192.168.1.2:13337")
    ep.tag_send(data, tag=42)
"""

from axon._core import (
    # Core classes
    Config,
    Context,
    Worker,
    Endpoint,
    Listener,
    MemoryRegion,
    # Futures
    FutureVoid,
    FutureRecv,
    # Enums
    MemoryType,
    ErrorCode,
    # Exceptions
    AXONError,
    # Constants
    TAG_ANY,
    TAG_MASK_ALL,
    TAG_MASK_USER,
    # Utility
    make_tag,
    tag_context,
    tag_user,
)

# Type aliases
Tag = int

__version__ = "0.1.0"

__all__ = [
    "Config",
    "Context",
    "Worker",
    "Endpoint",
    "Listener",
    "MemoryRegion",
    "FutureVoid",
    "FutureRecv",
    "Tag",
    "MemoryType",
    "ErrorCode",
    "AXONError",
    "TAG_ANY",
    "TAG_MASK_ALL",
    "TAG_MASK_USER",
    "make_tag",
    "tag_context",
    "tag_user",
]
