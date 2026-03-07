"""
Type stubs for the p2p._core C extension module (built via nanobind).

This file serves as the definitive Python API reference.
The actual _core.so is compiled from src/python/bindings.cpp.
"""

from __future__ import annotations

import enum
from typing import (
    Any,
    Awaitable,
    Callable,
    Optional,
    Sequence,
    Tuple,
    Union,
    overload,
)

import numpy as np

# Try to import cupy for type annotations; fall back to Any.
try:
    import cupy as cp
    GpuArray = cp.ndarray
except ImportError:
    GpuArray = Any  # type: ignore[misc]

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

TAG_ANY: int        # 0xFFFFFFFFFFFFFFFF
TAG_MASK_ALL: int   # 0xFFFFFFFFFFFFFFFF
TAG_MASK_USER: int  # 0x00000000FFFFFFFF

def make_tag(context_id: int, user_tag: int) -> int: ...
def tag_context(tag: int) -> int: ...
def tag_user(tag: int) -> int: ...

# ---------------------------------------------------------------------------
# Enums
# ---------------------------------------------------------------------------

class MemoryType(enum.IntEnum):
    HOST   = 0
    CUDA   = 1
    ROCM   = 2
    ASCEND = 3

class ErrorCode(enum.IntEnum):
    SUCCESS             = 0
    IN_PROGRESS         = 1
    CANCELED            = 2
    TIMEOUT             = 3
    CONNECTION_REFUSED  = 100
    CONNECTION_RESET    = 101
    ENDPOINT_CLOSED     = 102
    TRANSPORT_ERROR     = 200
    MESSAGE_TRUNCATED   = 201
    TAG_MISMATCH        = 202
    OUT_OF_MEMORY       = 300
    INVALID_BUFFER      = 301
    REGISTRATION_FAILED = 302
    PLUGIN_NOT_FOUND    = 400
    PLUGIN_INIT_FAILED  = 401
    INVALID_ARGUMENT    = 900
    INTERNAL_ERROR      = 999

class ReduceOp(enum.IntEnum):
    SUM  = 0
    PROD = 1
    MAX  = 2
    MIN  = 3
    AVG  = 4

class DataType(enum.IntEnum):
    FLOAT16  = 0
    FLOAT32  = 1
    FLOAT64  = 2
    BFLOAT16 = 3
    INT8     = 4
    INT32    = 5
    INT64    = 6
    UINT8    = 7

# ---------------------------------------------------------------------------
# Exceptions
# ---------------------------------------------------------------------------

class P2PError(Exception):
    """Base exception for all P2P errors."""
    code: ErrorCode
    message: str

class ConnectionError(P2PError): ...
class TransportError(P2PError): ...
class MemoryError(P2PError): ...
class TimeoutError(P2PError): ...

# ---------------------------------------------------------------------------
# Tag (type alias)
# ---------------------------------------------------------------------------

Tag = int

# ---------------------------------------------------------------------------
# Future[T] – awaitable async result
# ---------------------------------------------------------------------------

class Future:
    """
    Wraps an in-flight async operation.  Can be awaited in asyncio context
    or polled synchronously.

    Usage (sync):
        result = future.wait(timeout=5.0)

    Usage (async):
        result = await future
    """

    def ready(self) -> bool: ...
    def wait(self, timeout: Optional[float] = None) -> Any: ...
    def cancel(self) -> None: ...

    @property
    def status(self) -> ErrorCode: ...

    # asyncio support: makes Future awaitable
    def __await__(self): ...

# ---------------------------------------------------------------------------
# Request – low-level async handle
# ---------------------------------------------------------------------------

class Request:
    def is_complete(self) -> bool: ...
    def wait(self, timeout: Optional[float] = None) -> None: ...
    def cancel(self) -> None: ...

    @property
    def bytes_transferred(self) -> int: ...

    @property
    def matched_tag(self) -> int: ...

# ---------------------------------------------------------------------------
# MemoryRegion
# ---------------------------------------------------------------------------

class MemoryRegion:
    """
    A registered (pinned) memory region for zero-copy transfers.
    Implements the Python buffer protocol, so it can be used directly
    with numpy, cupy, memoryview, etc.
    """

    @staticmethod
    def register(
        ctx: Context,
        buffer: Union[bytes, bytearray, memoryview, np.ndarray, GpuArray],
        memory_type: MemoryType = MemoryType.HOST,
    ) -> MemoryRegion: ...

    @staticmethod
    def allocate(
        ctx: Context,
        size: int,
        memory_type: MemoryType = MemoryType.HOST,
    ) -> MemoryRegion: ...

    @property
    def address(self) -> int: ...

    @property
    def length(self) -> int: ...

    @property
    def memory_type(self) -> MemoryType: ...

    @property
    def remote_key(self) -> bytes: ...

    def __len__(self) -> int: ...

    # Buffer protocol: memoryview(region), np.frombuffer(region), etc.
    def __buffer__(self, flags: int) -> memoryview: ...

    def to_numpy(self) -> np.ndarray:
        """Zero-copy view as a uint8 numpy array (host memory only)."""
        ...

# ---------------------------------------------------------------------------
# MemoryPool
# ---------------------------------------------------------------------------

class MemoryPool:
    @staticmethod
    def create(
        ctx: Context,
        pool_bytes: int,
        memory_type: MemoryType = MemoryType.HOST,
    ) -> MemoryPool: ...

    def allocate(self, size: int) -> MemoryRegion: ...
    def deallocate(self, region: MemoryRegion) -> None: ...

    @property
    def total_bytes(self) -> int: ...

    @property
    def used_bytes(self) -> int: ...

    @property
    def free_bytes(self) -> int: ...

# ---------------------------------------------------------------------------
# RegistrationCache
# ---------------------------------------------------------------------------

class RegistrationCache:
    @staticmethod
    def create(ctx: Context, max_entries: int = 0) -> RegistrationCache: ...

    def get_or_register(
        self,
        buffer: Union[bytes, bytearray, memoryview, np.ndarray, GpuArray],
        memory_type: MemoryType = MemoryType.HOST,
    ) -> MemoryRegion: ...

    def invalidate(
        self,
        buffer: Union[bytes, bytearray, memoryview, np.ndarray, GpuArray],
    ) -> None: ...

    def flush(self) -> None: ...

    @property
    def size(self) -> int: ...
    @property
    def hits(self) -> int: ...
    @property
    def misses(self) -> int: ...

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

class Config:
    """
    Configuration for a P2P Context.  Use keyword arguments for a
    Pythonic construction style.

    Example:
        cfg = Config(
            transport="ucx",
            num_workers=4,
            memory_pool_size=256 * 1024 * 1024,
            registration_cache=True,
            ucx_tls="rc,ud,shm",
        )
    """

    def __init__(
        self,
        *,
        transport: str = "ucx",
        num_workers: int = 0,
        memory_pool_size: int = 64 * 1024 * 1024,
        max_inflight_requests: int = 1024,
        connect_timeout: float = 10.0,
        registration_cache: bool = True,
        registration_cache_max_entries: int = 0,
        **kwargs: str,   # arbitrary transport options, e.g. ucx_tls="rc"
    ) -> None: ...

    @property
    def transport(self) -> str: ...

    @property
    def num_workers(self) -> int: ...

    @property
    def memory_pool_size(self) -> int: ...

    def get(self, key: str, default: str = "") -> str: ...

# ---------------------------------------------------------------------------
# Context
# ---------------------------------------------------------------------------

class Context:
    """
    Top-level library handle.  Owns the UCX context and resource pools.

    Example:
        ctx = Context(transport="ucx")
        # or:
        ctx = Context(config=cfg)
    """

    @overload
    def __init__(self, *, config: Config) -> None: ...
    @overload
    def __init__(self, **kwargs: Any) -> None: ...

    def create_worker(self, index: int = 0) -> Worker: ...

    def supports_memory_type(self, mt: MemoryType) -> bool: ...
    def supports_rma(self) -> bool: ...
    def supports_hw_tag_matching(self) -> bool: ...

    @property
    def config(self) -> Config: ...

# ---------------------------------------------------------------------------
# Endpoint
# ---------------------------------------------------------------------------

class Endpoint:
    """
    A connection to a remote peer.  Supports tag-matched messaging,
    one-sided RDMA, and byte streams.

    All send/recv methods return awaitables usable with ``await``.
    They also accept any object implementing the buffer protocol
    (bytes, bytearray, memoryview, numpy array, cupy array).
    """

    # --- Tag-matched messaging ------------------------------------------------

    async def tag_send(
        self,
        buffer: Union[bytes, bytearray, memoryview, np.ndarray, GpuArray, MemoryRegion],
        tag: int,
        *,
        offset: int = 0,
        length: Optional[int] = None,
    ) -> None:
        """
        Send data with a tag.  Accepts any buffer-protocol object.

        For large messages (> ~8 KB), pre-registering via MemoryRegion
        enables true zero-copy RDMA rendezvous.
        """
        ...

    async def tag_recv(
        self,
        buffer: Union[bytearray, memoryview, np.ndarray, GpuArray, MemoryRegion],
        tag: int,
        *,
        tag_mask: int = TAG_MASK_ALL,
    ) -> Tuple[int, int]:
        """
        Receive data matching a tag into the provided buffer.

        Returns:
            (bytes_received, matched_tag)
        """
        ...

    # --- One-sided RDMA -------------------------------------------------------

    async def put(
        self,
        local_region: MemoryRegion,
        remote_addr: int,
        remote_key: bytes,
        *,
        local_offset: int = 0,
        length: Optional[int] = None,
    ) -> None:
        """RDMA write: local_region -> remote memory."""
        ...

    async def get(
        self,
        local_region: MemoryRegion,
        remote_addr: int,
        remote_key: bytes,
        *,
        local_offset: int = 0,
        length: Optional[int] = None,
    ) -> None:
        """RDMA read: remote memory -> local_region."""
        ...

    async def flush(self) -> None:
        """Ensure all pending RDMA operations are visible on the remote side."""
        ...

    # --- Stream ---------------------------------------------------------------

    async def stream_send(
        self,
        buffer: Union[bytes, bytearray, memoryview, np.ndarray, GpuArray, MemoryRegion],
    ) -> None: ...

    async def stream_recv(
        self,
        buffer: Union[bytearray, memoryview, np.ndarray, GpuArray, MemoryRegion],
    ) -> int:
        """Returns number of bytes received."""
        ...

    # --- Connection -----------------------------------------------------------

    async def close(self) -> None: ...

    @property
    def is_connected(self) -> bool: ...

    @property
    def remote_address(self) -> str: ...

# ---------------------------------------------------------------------------
# Listener
# ---------------------------------------------------------------------------

class Listener:
    @property
    def address(self) -> str: ...

    def close(self) -> None: ...

# ---------------------------------------------------------------------------
# Worker
# ---------------------------------------------------------------------------

class Worker:
    """
    The UCX progress engine.  Owns one ucp_worker_h.

    For asyncio integration the worker's event_fd is registered with the
    event loop so that ``progress()`` is called automatically whenever
    the transport signals readiness.
    """

    async def connect(self, address: str) -> Endpoint:
        """Connect to a remote peer and return an Endpoint."""
        ...

    def listen(
        self,
        bind_address: str,
        on_accept: Callable[[Endpoint], None],
    ) -> Listener: ...

    def progress(self) -> bool:
        """Drive the progress engine once (non-blocking)."""
        ...

    async def tag_recv(
        self,
        buffer: Union[bytearray, memoryview, np.ndarray, GpuArray, MemoryRegion],
        tag: int,
        *,
        tag_mask: int = TAG_MASK_ALL,
    ) -> Tuple[int, int]:
        """Any-source receive (matches any endpoint)."""
        ...

    @property
    def event_fd(self) -> int: ...

    @property
    def index(self) -> int: ...

    def attach_to_event_loop(self, loop: Optional[Any] = None) -> None:
        """
        Register this worker's event_fd with an asyncio event loop.
        When the fd is readable, the loop calls progress() automatically.
        If loop is None, uses asyncio.get_running_loop().
        """
        ...

    def detach_from_event_loop(self) -> None:
        """Unregister from the event loop."""
        ...

# ---------------------------------------------------------------------------
# Plugin registry
# ---------------------------------------------------------------------------

class PluginRegistry:
    @staticmethod
    def instance() -> PluginRegistry: ...

    def load(self, library_path: str) -> None: ...
    def discover(self, directory: str) -> None: ...
    def find(self, name: str) -> Any: ...
    def list(self) -> list[str]: ...
