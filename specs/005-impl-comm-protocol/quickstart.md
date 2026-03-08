# ZeroKV Communication Protocol Quickstart

## Running Tests

### Prerequisites

```bash
# Build ZeroKV
cd build
cmake .. -DBUILD_TESTS=ON
make -j$(nproc)
```

### Run Integration Tests

```bash
# Start server
./zerokv_server -a 127.0.0.1 -p 5000 -m 512 &

# Run Python integration tests
export PYTHONPATH=$PWD:$PYTHONPATH
pytest ../python/tests/test_zerokv.py -v -m integration
```

### Manual Testing

```python
import zerokv

# Connect to server
client = zerokv.Client()
client.connect(['127.0.0.1:5000'])

# Put operation
client.put('key', 'value')

# Get operation
value = client.get('key')
print(value)  # 'value'

# Delete operation
client.remove('key')
```

## Protocol Testing

### Message Format

Test protocol messages using hexdump:

```bash
# Monitor UCX traffic (debug)
UCX_LOG_LEVEL=debug ./zerokv_server
```

### Performance Testing

```python
import time
import zerokv

client = zerokv.Client()
client.connect(['127.0.0.1:5000'])

# Test latency
start = time.time()
for i in range(1000):
    client.put(f'key_{i}', f'value_{i}')
end = time.time()

print(f'1000 puts: {(end-start)*1000:.2f}ms')
```

## Troubleshooting

### Connection Failed

- Check server is running: `ps aux | grep zerokv_server`
- Verify port is open: `nc -zv localhost 5000`
- Check UCX status: `ucx_info -c`

### Protocol Errors

- Enable debug logging: `UCX_LOG_LEVEL=debug`
- Check server logs for malformed requests
- Verify message format matches spec in data-model.md
