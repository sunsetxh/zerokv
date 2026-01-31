"""
ZeroKV - Python Interface

High-performance NPU memory KV middleware with zero-copy transfer.
"""

__version__ = '1.0.0'

from typing import Union, Optional
import numpy as np

try:
    from .zerokv_native import ServerWrapper, ClientWrapper, MonitorWrapper
except ImportError as e:
    raise ImportError(
        "Failed to import zerokv_native. "
        "Please ensure the C++ extension is built. "
        "Run: pip install -e . from the python/ directory"
    ) from e

# Try importing torch, but don't fail if not available
try:
    import torch
    TORCH_AVAILABLE = True
except ImportError:
    TORCH_AVAILABLE = False

class Server:
    """ZeroKV Server

    Example:
        >>> server = Server(device_id=0)
        >>> server.start("0.0.0.0", 50051)
        >>> data = np.random.randn(1024, 1024).astype(np.float32)
        >>> server.put("my_key", data)
        >>> server.shutdown()
    """

    def __init__(self, device_id: int = 0):
        """Initialize server

        Args:
            device_id: NPU device ID (0-7)
        """
        self._server = ServerWrapper(device_id)

    def start(self, ip: str, port: int) -> None:
        """Start server

        Args:
            ip: IP address to listen on ("0.0.0.0" for all interfaces)
            port: Port to listen on

        Raises:
            RuntimeError: If server fails to start
        """
        self._server.start(ip, port)

    def put(self, key: str, data: Union[np.ndarray, 'torch.Tensor']) -> None:
        """Register NPU memory to KV store

        Args:
            key: Unique key identifier
            data: NumPy array or PyTorch Tensor

        Raises:
            RuntimeError: If put operation fails
            TypeError: If data type is not supported
        """
        if TORCH_AVAILABLE and hasattr(data, '__torch_function__'):
            # PyTorch Tensor
            if not data.is_npu:
                raise ValueError("PyTorch tensor must be on NPU device")
            self._server.put_torch(key, data)
        else:
            # NumPy array
            data_np = np.asarray(data)
            self._server.put(key, data_np)

    def delete(self, key: str) -> None:
        """Delete key from KV store

        Args:
            key: Key to delete

        Raises:
            RuntimeError: If delete operation fails
        """
        self._server.delete(key)

    def get_monitor(self) -> 'Monitor':
        """Get performance monitor

        Returns:
            Monitor object
        """
        return Monitor(self._server.get_monitor())

    def shutdown(self) -> None:
        """Shutdown server"""
        self._server.shutdown()


class Client:
    """ZeroKV Client

    Example:
        >>> client = Client(device_id=1)
        >>> client.connect("192.168.1.100", 50051)
        >>> data = client.get("my_key")
        >>> print(data.shape)
        >>> client.disconnect()
    """

    def __init__(self, device_id: int = 0):
        """Initialize client

        Args:
            device_id: Local NPU device ID
        """
        self._client = ClientWrapper(device_id)

    def connect(self, server_ip: str, port: int) -> None:
        """Connect to server

        Args:
            server_ip: Server IP address
            port: Server port

        Raises:
            RuntimeError: If connection fails
        """
        self._client.connect(server_ip, port)

    def get(self, key: str) -> np.ndarray:
        """Get data from server as NumPy array

        Args:
            key: Key to retrieve

        Returns:
            NumPy array (on CPU memory)

        Raises:
            RuntimeError: If get operation fails
        """
        return self._client.get(key)

    def get_torch(self, key: str) -> 'torch.Tensor':
        """Get data from server as PyTorch Tensor

        Args:
            key: Key to retrieve

        Returns:
            PyTorch Tensor (on NPU device)

        Raises:
            RuntimeError: If get operation fails
            ImportError: If PyTorch is not available
        """
        if not TORCH_AVAILABLE:
            raise ImportError("PyTorch is not available. Install with: pip install torch")
        return self._client.get_torch(key)

    def get_monitor(self) -> 'Monitor':
        """Get performance monitor

        Returns:
            Monitor object
        """
        return Monitor(self._client.get_monitor())

    def disconnect(self) -> None:
        """Disconnect from server"""
        self._client.disconnect()


class Monitor:
    """Performance Monitor

    Example:
        >>> monitor = server.get_monitor()
        >>> stats = monitor.get_stats('GET')
        >>> print(f"P95 Latency: {stats['p95_latency_us']} us")
    """

    def __init__(self, monitor_wrapper: MonitorWrapper):
        """Initialize monitor

        Args:
            monitor_wrapper: Native monitor wrapper object
        """
        self._monitor = monitor_wrapper

    def get_stats(self, operation: str) -> dict:
        """Get statistics for an operation type

        Args:
            operation: Operation type ('PUT', 'GET', 'DELETE')

        Returns:
            Dictionary with statistics:
                - total_ops: Total number of operations
                - success_ops: Number of successful operations
                - failed_ops: Number of failed operations
                - avg_latency_us: Average latency in microseconds
                - p50_latency_us: P50 latency
                - p95_latency_us: P95 latency
                - p99_latency_us: P99 latency
                - throughput_mbps: Throughput in MB/s
        """
        return self._monitor.get_stats(operation)

    def get_all_stats(self) -> dict:
        """Get statistics for all operation types

        Returns:
            Dictionary mapping operation type to statistics
        """
        return self._monitor.get_all_stats()

    def start_display(self) -> None:
        """Start real-time performance display in terminal"""
        self._monitor.start_display()

    def stop_display(self) -> None:
        """Stop real-time performance display"""
        self._monitor.stop_display()


__all__ = [
    'Server',
    'Client',
    'Monitor',
    '__version__',
]
