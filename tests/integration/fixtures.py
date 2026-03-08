"""
ZeroKV Integration Test Fixtures

Provides test fixtures for managing server lifecycle during integration tests.
"""

import subprocess
import time
import os
import signal
import sys
from typing import Optional, List


class TestServer:
    """Test server wrapper for managing ZeroKV server process"""

    def __init__(
        self,
        addr: str = "127.0.0.1",
        port: int = 5000,
        max_memory_mb: int = 512,
        server_path: Optional[str] = None
    ):
        self.addr = addr
        self.port = port
        self.max_memory_mb = max_memory_mb
        self.process: Optional[subprocess.Popen] = None
        self.server_path = server_path or self._find_server()

    def _find_server(self) -> str:
        """Find the zerokv_server binary"""
        # Check build directory
        build_path = os.path.join(os.path.dirname(__file__), "..", "..", "build", "zerokv_server")
        if os.path.exists(build_path):
            return os.path.abspath(build_path)

        # Check current directory
        if os.path.exists("./zerokv_server"):
            return os.path.abspath("./zerokv_server")

        raise FileNotFoundError("zerokv_server not found. Please build ZeroKV first.")

    def start(self, timeout: int = 10) -> bool:
        """Start the server and wait for it to be ready"""
        if self.process is not None:
            return True

        cmd = [
            self.server_path,
            "-a", self.addr,
            "-p", str(self.port),
            "-m", str(self.max_memory_mb)
        ]

        try:
            self.process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                preexec_fn=os.setsid
            )

            # Wait for server to be ready
            start_time = time.time()
            while time.time() - start_time < timeout:
                if self._is_ready():
                    return True
                time.sleep(0.1)

            # Timeout - check if process died
            if self.process.poll() is not None:
                stderr = self.process.stderr.read().decode() if self.process.stderr else ""
                raise RuntimeError(f"Server failed to start: {stderr}")

            return False

        except Exception as e:
            self.process = None
            raise RuntimeError(f"Failed to start server: {e}")

    def _is_ready(self) -> bool:
        """Check if server is ready to accept connections"""
        if self.process is None or self.process.poll() is not None:
            return False

        # Try to connect to check if server is ready
        try:
            import socket
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(1)
            result = sock.connect_ex((self.addr, self.port))
            sock.close()
            return result == 0
        except:
            return False

    def stop(self, timeout: int = 5) -> None:
        """Stop the server gracefully"""
        if self.process is None:
            return

        try:
            # Try graceful shutdown first
            os.killpg(os.getpgid(self.process.pid), signal.SIGTERM)

            # Wait for process to exit
            start_time = time.time()
            while time.time() - start_time < timeout:
                if self.process.poll() is not None:
                    break
                time.sleep(0.1)

            # Force kill if still running
            if self.process.poll() is None:
                os.killpg(os.getpgid(self.process.pid), signal.SIGKILL)
                self.process.wait(timeout=2)

        except ProcessLookupError:
            pass  # Process already dead
        finally:
            self.process = None

    def restart(self) -> bool:
        """Restart the server"""
        self.stop()
        return self.start()

    def __enter__(self):
        """Context manager entry"""
        self.start()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        """Context manager exit"""
        self.stop()

    def is_running(self) -> bool:
        """Check if server is running"""
        return self.process is not None and self.process.poll() is None


class TestClient:
    """Test client wrapper for ZeroKV"""

    def __init__(self, servers: Optional[List[str]] = None):
        import zerokv
        self.client = zerokv.Client()
        self.servers = servers or ["127.0.0.1:5000"]
        self._connected = False

    def connect(self, timeout: int = 5) -> None:
        """Connect to server"""
        start_time = time.time()
        while time.time() - start_time < timeout:
            try:
                self.client.connect(self.servers)
                self._connected = True
                return
            except Exception:
                time.sleep(0.1)

        raise RuntimeError(f"Failed to connect to {self.servers}")

    def disconnect(self) -> None:
        """Disconnect from server"""
        if self._connected:
            try:
                self.client.disconnect()
            except:
                pass
            self._connected = False

    def put(self, key: str, value: str) -> bool:
        """Put a key-value pair"""
        if not self._connected:
            self.connect()
        return self.client.put(key, value)

    def get(self, key: str) -> Optional[str]:
        """Get a value by key"""
        if not self._connected:
            self.connect()
        return self.client.get(key)

    def remove(self, key: str) -> bool:
        """Remove a key"""
        if not self._connected:
            self.connect()
        return self.client.remove(key)

    def batch_put(self, items: list) -> bool:
        """Batch put key-value pairs"""
        if not self._connected:
            self.connect()
        return self.client.batch_put(items)

    def batch_get(self, keys: list) -> list:
        """Batch get values by keys"""
        if not self._connected:
            self.connect()
        return self.client.batch_get(keys)

    def __enter__(self):
        """Context manager entry"""
        self.connect()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        """Context manager exit"""
        self.disconnect()


def create_test_server(port: int = 5000) -> TestServer:
    """Create a test server instance"""
    return TestServer(port=port)


def create_connected_client(servers: Optional[List[str]] = None) -> TestClient:
    """Create and connect a test client"""
    client = TestClient(servers)
    client.connect()
    return client
