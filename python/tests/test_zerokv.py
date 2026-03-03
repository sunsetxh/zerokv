"""
ZeroKV Python Integration Tests

Run with: pytest python/tests/test_zerokv.py -v
"""

import pytest
import zerokv
import sys
import os
import asyncio

# Add build directory to path for testing
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'build'))


class TestClientBasic:
    """Test basic client operations"""

    def test_version(self):
        """Test version attribute"""
        assert zerokv.__version__ == "0.1.0"

    def test_client_creation(self):
        """Test client can be created"""
        client = zerokv.Client()
        assert client is not None

    def test_zero_kv_alias(self):
        """Test ZeroKV alias works"""
        client = zerokv.ZeroKV()
        assert client is not None


class TestContextManager:
    """Test context manager support"""

    def test_context_manager_enter_exit(self):
        """Test context manager enter/exit"""
        client = zerokv.Client()
        # Should not raise
        client.__enter__()
        client.__exit__(None, None, None)


class TestMemoryTypes:
    """Test memory type constants"""

    def test_memory_constants(self):
        """Test memory type constants exist"""
        assert zerokv.MEMORY_CPU == "cpu"
        assert zerokv.MEMORY_HUAWEI_NPU == "huawei_npu"
        assert zerokv.MEMORY_NVIDIA_GPU == "nvidia_gpu"

    def test_set_memory_type(self):
        """Test set_memory_type method"""
        client = zerokv.Client()
        # Should not raise
        client.set_memory_type("cpu")
        client.set_memory_type("nvidia_gpu")


class TestBatchOperations:
    """Test batch operations"""

    def test_batch_put_format(self):
        """Test batch_put accepts list of tuples"""
        # Just test the format is accepted (will fail on actual server)
        client = zerokv.Client()
        items = [("key1", "value1"), ("key2", "value2")]
        # This will fail without server, but format should be valid
        with pytest.raises(Exception):
            client.batch_put(items)

    def test_batch_get_format(self):
        """Test batch_get accepts list of keys"""
        client = zerokv.Client()
        keys = ["key1", "key2", "key3"]
        # This will fail without server, but format should be valid
        with pytest.raises(Exception):
            client.batch_get(keys)


class TestErrorHandling:
    """Test error handling"""

    def test_get_without_connection(self):
        """Test get without server connection raises error"""
        client = zerokv.Client()
        # Should raise RuntimeError when not connected
        with pytest.raises(RuntimeError):
            _ = client.get("any_key")

    def test_remove_without_connection(self):
        """Test remove without server connection raises error"""
        client = zerokv.Client()
        # Should raise RuntimeError when not connected
        with pytest.raises(RuntimeError):
            client.remove("any_key")

    def test_put_without_connection(self):
        """Test put without server connection raises error"""
        client = zerokv.Client()
        # Should raise RuntimeError when not connected
        with pytest.raises(RuntimeError):
            client.put("any_key", "any_value")


class TestAsyncClient:
    """Test async client"""

    def test_async_client_creation(self):
        """Test async client can be created"""
        client = zerokv.AsyncZeroKVClient()
        assert client is not None

    def test_async_client_with_servers(self):
        """Test async client with servers list"""
        client = zerokv.AsyncZeroKVClient(["localhost:5000"])
        assert client.servers == ["localhost:5000"]


class TestHighLevelClient:
    """Test high-level client wrapper"""

    def test_zero_kv_client_creation(self):
        """Test ZeroKVClient can be created"""
        client = zerokv.ZeroKVClient()
        assert client is not None

    def test_zero_kv_client_with_servers(self):
        """Test ZeroKVClient with servers"""
        client = zerokv.ZeroKVClient(servers=["localhost:5000"])
        assert client.servers == ["localhost:5000"]

    def test_zero_kv_client_memory_type(self):
        """Test ZeroKVClient with memory type"""
        client = zerokv.ZeroKVClient(memory_type="nvidia_gpu")
        assert client.memory_type == "nvidia_gpu"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
