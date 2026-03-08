"""
Pytest configuration for ZeroKV integration tests

This file provides fixtures and configuration for running integration tests.
"""

import pytest
import sys
import os
import time

# Add build directory to path
build_path = os.path.join(os.path.dirname(__file__), "..", "..", "build")
if os.path.exists(build_path):
    sys.path.insert(0, build_path)

# Import fixtures
from tests.integration.fixtures import TestServer, TestClient


@pytest.fixture(scope="session")
def server_port():
    """Get a unique port for the test session"""
    return 5000


@pytest.fixture(scope="function")
def test_server(server_port):
    """Fixture that provides a test server for each test"""
    server = TestServer(port=server_port)
    server.start()
    yield server
    server.stop()


@pytest.fixture(scope="function")
def connected_client(test_server):
    """Fixture that provides a connected client"""
    client = TestClient(servers=[f"127.0.0.1:{test_server.port}"])
    client.connect()
    yield client
    client.disconnect()


@pytest.fixture(scope="session")
def server_address(server_port):
    """Fixture that provides server address"""
    return f"127.0.0.1:{server_port}"


def pytest_configure(config):
    """Pytest configuration hook"""
    # Add custom markers
    config.addinivalue_line(
        "markers", "integration: mark test as integration test (requires server)"
    )
    config.addinivalue_line(
        "markers", "slow: mark test as slow running"
    )
    config.addinivalue_line(
        "markers", "benchmark: mark test as benchmark"
    )


def pytest_collection_modifyitems(config, items):
    """Modify test collection"""
    # Add integration marker to tests in integration directory
    integration_path = os.path.join(os.path.dirname(__file__), "integration")
    for item in items:
        if "integration" in str(item.fspath):
            item.add_marker(pytest.mark.integration)
