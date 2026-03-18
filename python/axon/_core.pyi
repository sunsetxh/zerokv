"""
Type stubs for the axon._core C extension module (built via nanobind).

This file serves as the definitive Python API reference.
The actual _core.so is compiled from src/python/bindings.cpp.
"""

from __future__ import annotations

import enum
from typing import Any, Callable, Optional, Tuple, Union, overload

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

# ---------------------------------------------------------------------------
# Exceptions
# ---------------------------------------------------------------------------

class AXONError(Exception):
    """Base exception for all AXON errors."""
    code: ErrorCode
    message: str

class FutureVoid:
    def ready(self) -> bool: ...
    def get(self, timeout: Optional[float] = None) -> None: ...
    def cancel(self) -> None: ...

    @property
    def status(self) -> Any: ...

class FutureRecv:
    def ready(self) -> bool: ...
    def get(self, timeout: Optional[float] = None) -> Tuple[int, int]: ...
    def cancel(self) -> None: ...

    @property
    def status(self) -> Any: ...

# ---------------------------------------------------------------------------
# MemoryRegion
# ---------------------------------------------------------------------------

class MemoryRegion:
    """
    A registered (pinned) memory region for zero-copy transfers.
    """

    @staticmethod
    def register_(
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

    def to_numpy(self) -> np.ndarray:
        ...

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

class Config:
    """
    Configuration for a AXON Context.
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
    Top-level library handle.
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
    A connection to a remote peer.
    """

    def tag_send(
        self,
        buffer: Union[bytes, bytearray, memoryview, np.ndarray, GpuArray],
        tag: int,
    ) -> FutureVoid: ...
    def tag_send_region(self, region: MemoryRegion, tag: int) -> FutureVoid: ...
    def tag_recv(
        self,
        buffer: Union[bytes, bytearray, memoryview, np.ndarray, GpuArray],
        tag: int,
        tag_mask: int = TAG_MASK_ALL,
    ) -> FutureRecv: ...
    def flush(self) -> FutureVoid: ...
    def close(self) -> FutureVoid: ...

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
    The UCX progress engine.
    """

    def connect(self, address: str) -> Any: ...
    def connect_blob(self, address: list[int]) -> Any: ...

    def listen(
        self,
        bind_address: str,
        on_accept: Callable[[Endpoint], None],
    ) -> Listener: ...

    def progress(self) -> bool:
        """Drive the progress engine once (non-blocking)."""
        ...

    @property
    def event_fd(self) -> int: ...

    @property
    def index(self) -> int: ...

    def attach_to_event_loop(self, loop: Optional[Any] = None) -> None:
        """
        """
        ...

    def detach_from_event_loop(self) -> None:
        """Unregister from the event loop."""
        ...

    # Background progress thread for true async operation
    def start_progress_thread(self) -> None:
        """
        Start a background thread that continuously drives progress.
        This enables truly asynchronous operation - await/get won't block.
        """
        ...

    def stop_progress_thread(self) -> None:
        """Stop the background progress thread."""
        ...

    @property
    def progress_thread_running(self) -> bool:
        """Check if background progress thread is running."""
        ...

Tag = int
