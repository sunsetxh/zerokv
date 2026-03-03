"""ZeroKV Async Python Client"""

import asyncio
from typing import List


class AsyncZeroKVClient:
    """Async ZeroKV client using asyncio"""

    def __init__(self, servers: List[str] = None):
        self.servers = servers or ["localhost:5000"]
        self._client = None

    async def connect(self):
        """Connect to cluster"""
        from _zerokv import ZeroKV
        self._client = ZeroKV()
        self._client.connect(self.servers)

    async def put(self, key: str, value: str):
        """Put key-value"""
        loop = asyncio.get_event_loop()
        await loop.run_in_executor(None, self._client.put, key, value)

    async def get(self, key: str) -> str:
        """Get value"""
        loop = asyncio.get_event_loop()
        return await loop.run_in_executor(None, self._client.get, key)

    async def batch_put(self, items: dict):
        """Batch put"""
        for key, value in items.items():
            await self.put(key, value)

    async def batch_get(self, keys: List[str]) -> List[str]:
        """Batch get"""
        results = []
        for key in keys:
            results.append(await self.get(key))
        return results

    async def close(self):
        """Close connection"""
        if self._client:
            self._client.disconnect()


async def main():
    client = AsyncZeroKVClient(["localhost:5000"])
    await client.connect()

    await client.put("key1", "value1")
    value = await client.get("key1")
    print(f"Got: {value}")

    await client.close()


if __name__ == "__main__":
    asyncio.run(main())
