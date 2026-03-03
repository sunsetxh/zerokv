"""
ZeroKV - High-performance distributed KV store for AI training

Quick Start:
    import zerokv

    # Create client
    client = zerokv.Client()

    # Connect to server
    client.connect(["127.0.0.1:5000"])

    # Put and get values
    client.put("key1", "value1")
    value = client.get("key1")

    # Or use context manager
    with zerokv.Client() as client:
        client.put("key2", "value2")
        value = client.get("key2")

    # Batch operations
    items = [("key1", "value1"), ("key2", "value2")]
    client.batch_put(items)
    values = client.batch_get(["key1", "key2"])

    # Async client
    import asyncio
    async def main():
        async with zerokv.AsyncClient() as client:
            await client.put("key", "value")
            value = await client.get("key")

    asyncio.run(main())
"""

import os
import sys

# Add the directory containing _zerokv.so to path
_package_dir = os.path.dirname(__file__)
if _package_dir not in sys.path:
    sys.path.insert(0, _package_dir)

from _zerokv import Client, ZeroKV, MEMORY_CPU, MEMORY_HUAWEI_NPU, MEMORY_NVIDIA_GPU
from .client import ZeroKVClient
from .async_client import AsyncZeroKVClient

__version__ = "0.1.0"
__all__ = [
    "Client",
    "ZeroKV",
    "ZeroKVClient",
    "AsyncZeroKVClient",
    "MEMORY_CPU",
    "MEMORY_HUAWEI_NPU",
    "MEMORY_NVIDIA_GPU"
]
