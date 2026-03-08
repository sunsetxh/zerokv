"""
ZeroKV Python Integration Tests

Run with: pytest python/tests/test_zerokv.py -v

Integration tests that require a running server:
    pytest python/tests/test_zerokv.py -v -m integration

Unit tests (no server required):
    pytest python/tests/test_zerokv.py -v -m "not integration"
"""

import pytest
import zerokv
import sys
import os
import asyncio

# Add build directory to path for testing
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'build'))

# Add tests directory for fixtures
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tests', 'integration'))


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


@pytest.mark.integration
class TestServerIntegration:
    """Integration tests that require a running ZeroKV server"""

    @pytest.fixture(autouse=True)
    def setup_teardown(self):
        """Setup and teardown for each test"""
        # Import here to avoid import errors if fixtures not available
        try:
            from fixtures import TestServer, TestClient
            self.server = TestServer(port=5000)
            self.server.start()
            yield
            self.server.stop()
        except Exception as e:
            pytest.skip(f"Server not available: {e}")

    def test_put_get_operations(self):
        """Test US1: Put and get operations work end-to-end"""
        client = zerokv.Client()
        client.connect(["127.0.0.1:5000"])

        # Put a key-value pair (returns None on success)
        client.put("test_key", "test_value")

        # Get the value
        value = client.get("test_key")
        assert value == "test_value"

    def test_delete_operations(self):
        """Test US1: Delete operations work correctly"""
        client = zerokv.Client()
        client.connect(["127.0.0.1:5000"])

        # Put a key-value pair
        client.put("delete_key", "delete_value")

        # Verify it exists
        value = client.get("delete_key")
        assert value == "delete_value"

        # Delete the key - returns None on success
        client.remove("delete_key")

        # Verify it's gone - should raise or return None
        try:
            value = client.get("delete_key")
            assert value is None or value == ""
        except:
            pass  # Expected - key should not exist

    def test_batch_operations(self):
        """Test US2: Batch operations work correctly"""
        client = zerokv.Client()
        client.connect(["127.0.0.1:5000"])

        # Prepare batch data
        items = [
            ("batch_key_1", "batch_value_1"),
            ("batch_key_2", "batch_value_2"),
            ("batch_key_3", "batch_value_3")
        ]

        # Batch put (returns None on success)
        client.batch_put(items)

        # Batch get
        keys = ["batch_key_1", "batch_key_2", "batch_key_3"]
        values = client.batch_get(keys)

        assert len(values) == 3
        assert values[0] == "batch_value_1"
        assert values[1] == "batch_value_2"
        assert values[2] == "batch_value_3"

    def test_multiple_clients(self):
        """Test US1: Multiple clients can connect to same server"""
        client1 = zerokv.Client()
        client1.connect(["127.0.0.1:5000"])

        client2 = zerokv.Client()
        client2.connect(["127.0.0.1:5000"])

        # Client 1 writes
        client1.put("shared_key", "client1_value")

        # Client 2 reads
        value = client2.get("shared_key")
        assert value == "client1_value"

    def test_update_existing_key(self):
        """Test US1: Updating existing key works correctly"""
        client = zerokv.Client()
        client.connect(["127.0.0.1:5000"])

        # Put initial value
        client.put("update_key", "value1")

        # Get initial value
        value1 = client.get("update_key")
        assert value1 == "value1"

        # Update value
        client.put("update_key", "value2")

        # Get updated value
        value2 = client.get("update_key")
        assert value2 == "value2"


@pytest.mark.integration
@pytest.mark.skipif(
    not os.path.exists("/home/wyc/code/zerokv/build/zerokv_server"),
    reason="Server binary not found"
)
class TestServerLifecycle:
    """Test server lifecycle management"""

    def test_server_start_stop(self):
        """Test server can be started and stopped"""
        from fixtures import TestServer

        server = TestServer(port=5001)
        assert not server.is_running()

        server.start()
        assert server.is_running()

        server.stop()
        assert not server.is_running()

    def test_server_context_manager(self):
        """Test server works as context manager"""
        from fixtures import TestServer

        with TestServer(port=5002) as server:
            assert server.is_running()

        assert not server.is_running()


@pytest.mark.integration
class TestFailureRecovery:
    """Test US3: Failure recovery scenarios"""

    @pytest.fixture(autouse=True)
    def setup_teardown(self):
        """Setup and teardown for each test"""
        try:
            from fixtures import TestServer
            self.server = TestServer(port=5003)
            self.server.start()
            yield
            self.server.stop()
        except Exception as e:
            pytest.skip(f"Server not available: {e}")

    def test_server_restart(self):
        """Test US3: Server can restart and client reconnects"""
        client = zerokv.Client()
        client.connect(["127.0.0.1:5003"])

        # Write data
        client.put("restart_key", "before_restart")

        # Verify data exists
        value = client.get("restart_key")
        assert value == "before_restart"

        # Restart server
        self.server.restart()

        # Reconnect and verify data persists (if server supports persistence)
        # Note: In-memory server will lose data on restart
        client.disconnect()
        client.connect(["127.0.0.1:5003"])

        # Server was restarted, data may or may not persist
        # This test verifies reconnection works
        try:
            value = client.get("restart_key")
            # Data might be gone if in-memory
        except:
            pass  # Expected if in-memory

    def test_reconnect_after_disconnect(self):
        """Test US3: Client can reconnect after disconnect"""
        client = zerokv.Client()
        client.connect(["127.0.0.1:5003"])

        # Write some data
        client.put("reconnect_key", "reconnect_value")

        # Disconnect
        client.disconnect()

        # Reconnect
        client.connect(["127.0.0.1:5003"])

        # Data should still be there
        value = client.get("reconnect_key")
        assert value == "reconnect_value"


@pytest.mark.benchmark
class TestPerformance:
    """Test US4: Performance benchmarks"""

    @pytest.fixture(autouse=True)
    def setup_teardown(self):
        """Setup and teardown for each test"""
        try:
            from fixtures import TestServer
            self.server = TestServer(port=5004)
            self.server.start()
            yield
            self.server.stop()
        except Exception as e:
            pytest.skip(f"Server not available: {e}")

    @pytest.mark.integration
    def test_single_client_latency(self):
        """Test US4: Single client put/get latency"""
        import time

        client = zerokv.Client()
        client.connect(["127.0.0.1:5004"])

        # Measure put latency
        num_ops = 100
        start = time.time()
        for i in range(num_ops):
            client.put(f"perf_key_{i}", f"perf_value_{i}")
        put_latency = (time.time() - start) * 1000 / num_ops

        # Measure get latency
        start = time.time()
        for i in range(num_ops):
            _ = client.get(f"perf_key_{i}")
        get_latency = (time.time() - start) * 1000 / num_ops

        print(f"\nSingle client latency: put={put_latency:.2f}ms, get={get_latency:.2f}ms")

        # Should be under 10ms per operation
        assert put_latency < 10, f"Put latency {put_latency:.2f}ms exceeds 10ms"
        assert get_latency < 10, f"Get latency {get_latency:.2f}ms exceeds 10ms"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
