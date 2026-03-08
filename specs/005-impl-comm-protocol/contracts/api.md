# ZeroKV Client API Contract

## C++ Client API

### Connection

```cpp
// Connect to server
Status Client::connect(const std::vector<std::string>& servers);

// Disconnect from server
void Client::disconnect();
```

### Operations

```cpp
// Put key-value
Status Client::put(const std::string& key, const std::string& value);

// Get value by key
std::string Client::get(const std::string& key);

// Delete key
Status Client::remove(const std::string& key);

// Batch put
Status Client::batch_put(const std::vector<std::pair<std::string, std::string>>& items);

// Batch get
std::vector<std::string> Client::batch_get(const std::vector<std::string>& keys);
```

### Memory Types

```cpp
// Set memory type for RDMA operations
void Client::set_memory_type(MemoryType type);
```

## Python Client API

```python
# Client class
class Client:
    def connect(self, servers: List[str]) -> None:
        """Connect to ZeroKV servers"""

    def disconnect(self) -> None:
        """Disconnect from servers"""

    def put(self, key: str, value: str) -> None:
        """Store key-value pair"""

    def get(self, key: str) -> str:
        """Get value by key"""

    def remove(self, key: str) -> None:
        """Remove key"""

    def batch_put(self, items: List[Tuple[str, str]]) -> None:
        """Batch store key-value pairs"""

    def batch_get(self, keys: List[str]) -> List[str]:
        """Batch get values by keys"""
```

## Server Protocol

### Message Format

See `data-model.md` for binary message format.

### Request Flow

1. Client establishes UCX connection to server
2. Client sends binary request message
3. Server processes request
4. Server sends binary response message
5. Client handles response
