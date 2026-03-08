# Protocol Data Model

## Message Format

### Request Message

```
+----------+--------+----------+-----------+-----------+------------+
| op_code  | flags  | key_len  | value_len |    key    |   value    |
| (1 byte) |(1 byte)| (4 bytes)| (4 bytes) |(key_len)  | (value_len)|
+----------+--------+----------+-----------+-----------+------------+
```

### Response Message

```
+----------+--------+-----------+------------+
|  status  | flags  | value_len |   value   |
| (1 byte) |(1 byte)| (4 bytes)|(value_len)|
+----------+--------+-----------+------------+
```

## Enums

### Operation Codes

| Code  | Name      | Description           |
|-------|-----------|----------------------|
| 0x01  | PUT       | Store key-value      |
| 0x02  | GET       | Retrieve value       |
| 0x03  | DELETE    | Remove key           |
| 0x04  | BATCH_PUT| Batch store         |
| 0x05  | BATCH_GET| Batch retrieve      |

### Status Codes

| Code | Name      | Description           |
|------|-----------|----------------------|
| 0x00 | OK        | Operation succeeded   |
| 0x01 | NOT_FOUND| Key not found        |
| 0x02 | ERROR     | Operation failed     |

### Flag Bits

| Bit | Name      | Description              |
|-----|-----------|-------------------------|
| 0   | COMPRESS  | Value is compressed     |
| 1   | CHECKSUM | Include checksum       |
| 2   | SYNC     | Synchronous operation   |

## Message Examples

### PUT Request

```
op_code: 0x01 (PUT)
flags: 0x00
key_len: 4
value_len: 5
key: "test"
value: "value"
```

### PUT Response

```
status: 0x00 (OK)
flags: 0x00
value_len: 0
value: (empty)
```

### GET Request

```
op_code: 0x02 (GET)
flags: 0x00
key_len: 4
value_len: 0
key: "test"
value: (empty)
```

### GET Response (Success)

```
status: 0x00 (OK)
flags: 0x00
value_len: 5
value: "value"
```

### GET Response (Not Found)

```
status: 0x01 (NOT_FOUND)
flags: 0x00
value_len: 0
value: (empty)
```

## Connection Flow

1. Client connects to server via UCX
2. Client sends request message
3. Server processes request using Storage engine
4. Server sends response message
5. Client receives and decodes response
