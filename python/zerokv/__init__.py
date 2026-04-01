"""
zerokv - High-performance AXON transport and RDMA KV library with Python bindings.

Usage:
    import zerokv

    ctx = zerokv.Context(transport="ucx", num_workers=2)
    worker = ctx.create_worker()
    status = zerokv.KVServer().start("0.0.0.0:15000")
    status.throw_if_error()
"""

from zerokv._core import (
    # Core classes
    Config,
    Status,
    Context,
    Worker,
    Endpoint,
    Listener,
    MemoryRegion,
    KVServer,
    KVNode,
    KeyInfo,
    FetchResult,
    PublishMetrics,
    FetchMetrics,
    PushMetrics,
    SubscriptionEvent,
    # Futures
    FutureVoid,
    FutureSize,
    FutureRecv,
    FutureU64,
    FutureEndpoint,
    FutureFetch,
    # Enums
    MemoryType,
    ErrorCode,
    SubscriptionEventType,
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
    "Status",
    "Context",
    "Worker",
    "Endpoint",
    "Listener",
    "MemoryRegion",
    "KVServer",
    "KVNode",
    "KeyInfo",
    "FetchResult",
    "PublishMetrics",
    "FetchMetrics",
    "PushMetrics",
    "SubscriptionEvent",
    "FutureVoid",
    "FutureSize",
    "FutureRecv",
    "FutureU64",
    "FutureEndpoint",
    "FutureFetch",
    "Tag",
    "MemoryType",
    "ErrorCode",
    "SubscriptionEventType",
    "AXONError",
    "TAG_ANY",
    "TAG_MASK_ALL",
    "TAG_MASK_USER",
    "make_tag",
    "tag_context",
    "tag_user",
]
