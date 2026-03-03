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
# Run all integration tests
pytest python/tests/test_zerokv.py -v

# Run specific test
pytest python/tests/test_zerokv.py::TestClientBasic -v
```

### Run C++ Integration Tests

```bash
# Build and run tests
cd build
ctest --output-on-failure

# Run specific test
./tests/test_storage
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

## Adding New Integration Tests

### Python

```python
# tests/integration/test_server.py
import pytest
import zerokv

def test_basic_operations():
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

TEST(IntegrationTest, BasicOperations) {
    zerokv::Client client;
    client.connect({"localhost:5000"});

    Status status = client.put("test_key", "test_value", 10);
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
