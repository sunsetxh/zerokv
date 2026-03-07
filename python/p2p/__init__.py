"""
p2p - High-performance P2P transport library with Python bindings.

Usage:
    import p2p

    ctx = p2p.Context(transport="ucx", num_workers=2)
    worker = ctx.create_worker()
    ep = worker.connect("192.168.1.2:13337")
    ep.tag_send(data, tag=42)
"""

from p2p._core import (
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
    P2PError,
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
    "P2PError",
    "TAG_ANY",
    "TAG_MASK_ALL",
    "TAG_MASK_USER",
    "make_tag",
    "tag_context",
    "tag_user",
]
