"""
ZeroKV Python Usage Examples

Install:
    pip install zerokv

Or build from source:
    cd python && pip install .
"""

from zerokv import ZeroKV


def example_basic():
    """Basic put/get operations"""
    print("=== Example 1: Basic Operations ===")

    client = ZeroKV()
    client.connect(["localhost:5000"])

    # Put
    client.put("user:1001", '{"name": "Alice", "age": 30}')

    # Get
    value = client.get("user:1001")
    print(f"Got: {value}")

    client.disconnect()


def example_batch():
    """Batch operations"""
    print("\n=== Example 2: Batch Operations ===")

    client = ZeroKV()
    client.connect(["localhost:5000"])

    # Batch put
    for i in range(100):
        client.put(f"key_{i}", f"value_{i}")

    print("Inserted 100 items")

    # Batch get
    for i in range(0, 10, 2):
        value = client.get(f"key_{i}")
        print(f"key_{i} = {value}")

    client.disconnect()


def example_user_memory():
    """User memory operations (RDMA zero-copy)"""
    print("\n=== Example 3: User Memory ===")

    import numpy as np
    client = ZeroKV()
    client.connect(["localhost:5000"])
    client.set_memory_type("nvidia_gpu")

    # Create GPU array
    arr = np.zeros(1024 * 1024, dtype=np.float32)

    # Put with user memory (would use RDMA in production)
    # client.put_to_buffer("large_array", arr)

    print("GPU memory operations prepared")

    client.disconnect()


def example_async():
    """Async operations"""
    print("\n=== Example 4: Async Operations ===")

    import asyncio
    from zerokv.async_client import AsyncZeroKVClient

    async def main():
        client = AsyncZeroKVClient(["localhost:5000"])
        await client.connect()

        # Async operations
        await client.put("key1", "value1")
        value = await client.get("key1")
        print(f"Got: {value}")

        await client.close()

    asyncio.run(main())


if __name__ == "__main__":
    example_basic()
    example_batch()
    # example_user_memory()  # Requires GPU
    # example_async()  # Requires async support
