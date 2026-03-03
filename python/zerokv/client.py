"""ZeroKV Python Client - High-level API"""


class ZeroKVClient:
    def __init__(self, servers=None, memory_type="cpu"):
        self.servers = servers or ["localhost:5000"]
        self.memory_type = memory_type
        self._client = None

    def connect(self):
        """Connect to ZeroKV cluster"""
        from _zerokv import ZeroKV
        self._client = ZeroKV()
        self._client.connect(self.servers)
        self._client.set_memory_type(self.memory_type)

    def disconnect(self):
        """Disconnect from cluster"""
        if self._client:
            self._client.disconnect()

    def put(self, key, value):
        """Put key-value pair"""
        if not self._client:
            raise RuntimeError("Not connected")
        self._client.put(key, value)

    def get(self, key):
        """Get value by key"""
        if not self._client:
            raise RuntimeError("Not connected")
        return self._client.get(key)

    def remove(self, key):
        """Remove key"""
        if not self._client:
            raise RuntimeError("Not connected")
        self._client.remove(key)

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *args):
        self.disconnect()
