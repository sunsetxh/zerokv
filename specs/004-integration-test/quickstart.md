# ZeroKV Integration Tests Quickstart

## Running Integration Tests

### Prerequisites

```bash
# Build ZeroKV first
cd build
cmake .. -DBUILD_TESTS=ON
make -j$(nproc)
```

### Run Python Integration Tests

```bash
# Run all unit tests (no server required)
pytest python/tests/test_zerokv.py -v -m "not integration"

# Run all tests including integration tests (requires server)
pytest python/tests/test_zerokv.py -v

# Run only integration tests
pytest python/tests/test_zerokv.py -v -m integration

# Run specific test class
pytest python/tests/test_zerokv.py::TestClientBasic -v

# Run integration tests with server lifecycle management
pytest python/tests/test_zerokv.py::TestServerIntegration -v

# Run performance benchmarks
pytest python/tests/test_zerokv.py -v -m benchmark
```

### Run C++ Integration Tests

```bash
# Build and run tests
cd build
ctest --output-on-failure

# Run specific test
./tests/test_storage

# Run integration tests (if built)
./tests/test_cluster
```

### Run Performance Benchmarks

```bash
# Python benchmark
python -c "
import time
import zerokv

client = zerokv.Client()
client.connect(['localhost:5000'])

start = time.time()
for i in range(1000):
    client.put(f'key_{i}', f'value_{i}')
end = time.time()

print(f'1000 puts: {(end-start)*1000:.2f}ms')
"
```

## Test Fixtures

### Python Test Fixtures

The integration test framework provides fixtures in `tests/integration/fixtures.py`:

```python
from tests.integration.fixtures import TestServer, TestClient

# Using TestServer as context manager
with TestServer(port=5000) as server:
    # Server is running
    client = TestClient(servers=[f"127.0.0.1:{server.port}"])
    client.connect()
    client.put("key", "value")
    value = client.get("key")
    assert value == "value"
    # Server automatically stopped on exit
```

### C++ Test Fixtures

The C++ test fixtures are in `tests/integration/test_server_fixture.h`:

```cpp
#include "test_server_fixture.h"

// Using GoogleTest fixture
class MyIntegrationTest : public ConnectedClientTest {};

// Tests automatically get a running server and connected client
TEST_F(MyIntegrationTest, PutGet) {
    Status status = client_->Put("key", "value");
    EXPECT_EQ(status, Status::OK);

    std::string value = client_->Get("key");
    EXPECT_EQ(value, "value");
}
```

## Adding New Integration Tests

### Python

```python
# tests/integration/test_server.py
import pytest
import zerokv

@pytest.mark.integration
class TestMyFeature:
    """Test my feature"""

    def test_basic_operations(self):
        """Test basic put/get operations"""
        client = zerokv.Client()
        client.connect(['localhost:5000'])

        client.put('test_key', 'test_value')
        value = client.get('test_key')

        assert value == 'test_value'
```

### C++

```cpp
// tests/integration/test_cluster.cc
#include <gtest/gtest.h>
#include "zerokv/client.h"
#include "test_server_fixture.h"

TEST(IntegrationTest, BasicOperations) {
    test::TestClient client({"localhost:5000"});
    client.Connect();

    Status status = client.Put("test_key", "test_value");
    ASSERT_EQ(status, Status::OK);
}
```

## CI/CD Integration

### GitHub Actions Example

```yaml
name: Integration Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Build
        run: |
          mkdir build && cd build
          cmake .. -DBUILD_TESTS=ON
          make -j$(nproc)
      - name: Run Tests
        run: |
          cd build
          ctest --output-on-failure
      - name: Python Tests
        run: pytest python/tests/ -v
```

## Troubleshooting

### Server won't start

- Check port 5000 is available
- Verify UCX is properly installed
- Check server logs for errors

### Connection refused

- Ensure server is running: `./zerokv_server`
- Check firewall settings
- Verify port matches between client and server

### Tests timeout

- Increase timeout in conftest.py
- Check system resources
- Verify network latency

### UCX errors

- Verify UCX is installed: `ucx_info -c`
- Check network interfaces are available
- Try running server with `-vv` for verbose output
