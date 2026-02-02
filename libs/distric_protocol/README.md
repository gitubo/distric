# DistriC Protocol Layer

Binary protocol implementation with fixed 32-byte headers, CRC32 checksums, and network byte order serialization.

## Features

- **Fixed 32-byte header**: Predictable, cache-friendly message structure
- **Network byte order**: Big-endian serialization for cross-platform compatibility
- **CRC32 checksums**: IEEE 802.3 polynomial for corruption detection
- **Message type system**: Organized by subsystem (Raft, Gossip, Task, Client)
- **Zero dependencies**: Only standard C library + distric_obs
- **Portable**: Tested on little-endian and big-endian systems

## Quick Start

### Include

```c
#include <distric_protocol.h>
```

### Create and Send Message

```c
// Initialize header
message_header_t header;
message_header_init(&header, MSG_RAFT_REQUEST_VOTE, payload_len);

// Create payload
uint8_t payload[1024];
// ... fill payload ...

// Compute CRC32
compute_header_crc32(&header, payload, payload_len);

// Serialize header
uint8_t wire_buffer[MESSAGE_HEADER_SIZE];
serialize_header(&header, wire_buffer);

// Send: wire_buffer (32 bytes) followed by payload
send(socket, wire_buffer, MESSAGE_HEADER_SIZE, 0);
send(socket, payload, payload_len, 0);
```

### Receive and Validate Message

```c
// Receive header
uint8_t wire_buffer[MESSAGE_HEADER_SIZE];
recv(socket, wire_buffer, MESSAGE_HEADER_SIZE, 0);

// Deserialize
message_header_t header;
deserialize_header(wire_buffer, &header);

// Validate header
if (!validate_message_header(&header)) {
    // Invalid header
    return;
}

// Receive payload
uint8_t* payload = malloc(header.payload_len);
recv(socket, payload, header.payload_len, 0);

// Verify CRC32
if (!verify_message_crc32(&header, payload, header.payload_len)) {
    // Corruption detected
    free(payload);
    return;
}

// Process message
printf("Message type: %s\n", message_type_to_string(header.msg_type));
// ...

free(payload);
```

## Message Header Structure

```
┌─────────────────────────────────────────┐
│  Offset | Size | Field                  │
├─────────┼──────┼────────────────────────┤
│  0      | 4    | Magic (0x44495354)     │
│  4      | 2    | Version (0x0001)       │
│  6      | 2    | Message Type           │
│  8      | 2    | Flags                  │
│  10     | 2    | Reserved               │
│  12     | 4    | Payload Length         │
│  16     | 8    | Message ID             │
│  24     | 4    | Timestamp (seconds)    │
│  28     | 4    | CRC32                  │
└─────────────────────────────────────────┘
Total: 32 bytes (packed, network byte order)
```

## Message Types

### Raft Consensus (0x1xxx)
- `MSG_RAFT_REQUEST_VOTE` (0x1001)
- `MSG_RAFT_APPEND_ENTRIES` (0x1003)
- `MSG_RAFT_INSTALL_SNAPSHOT` (0x1005)

### Gossip Protocol (0x2xxx)
- `MSG_GOSSIP_PING` (0x2001)
- `MSG_GOSSIP_ACK` (0x2002)
- `MSG_GOSSIP_MEMBERSHIP_UPDATE` (0x2004)

### Task Execution (0x3xxx)
- `MSG_TASK_ASSIGNMENT` (0x3001)
- `MSG_TASK_RESULT` (0x3002)
- `MSG_TASK_STATUS` (0x3003)

### Client API (0x4xxx)
- `MSG_CLIENT_SUBMIT` (0x4001)
- `MSG_CLIENT_RESPONSE` (0x4002)

## Message Flags

```c
MSG_FLAG_NONE           // No flags
MSG_FLAG_COMPRESSED     // Payload is compressed
MSG_FLAG_ENCRYPTED      // Payload is encrypted
MSG_FLAG_URGENT         // High-priority message
MSG_FLAG_RETRY          // This is a retry
MSG_FLAG_RESPONSE       // This is a response
```

## API Reference

### Header Initialization

```c
distric_err_t message_header_init(
    message_header_t* header,
    message_type_t msg_type,
    uint32_t payload_len
);
```

Initializes header with magic, version, unique message_id, and timestamp.

### Serialization

```c
distric_err_t serialize_header(
    const message_header_t* header,
    uint8_t* buffer
);
```

Converts header to network byte order (big-endian).

### Deserialization

```c
distric_err_t deserialize_header(
    const uint8_t* buffer,
    message_header_t* header
);
```

Converts from network byte order to host byte order.

### Validation

```c
bool validate_message_header(const message_header_t* header);
```

Checks magic, version, message type, and payload length.

### CRC32

```c
distric_err_t compute_header_crc32(
    message_header_t* header,
    const uint8_t* payload,
    size_t payload_len
);

bool verify_message_crc32(
    const message_header_t* header,
    const uint8_t* payload,
    size_t payload_len
);
```

Compute or verify CRC32 checksum over header + payload.

## Build

From project root:

```bash
# Configure
cmake -B build -DBUILD_TESTING=ON

# Build
cmake --build build

# Run tests
cd build && ctest -R test_binary --output-on-failure
```

## Testing

Run comprehensive test suite:

```bash
./build/libs/distric_protocol/tests/test_binary
```

Tests verify:
- Header size (exactly 32 bytes)
- Serialization/deserialization round-trip
- Network byte order conversion
- CRC32 computation and verification
- Corruption detection (single-bit errors)
- Message validation
- Unique message ID generation
- Portability across endianness

## Performance Characteristics

- **Header serialization**: ~50-100 ns
- **CRC32 computation**: ~5-10 cycles/byte (table-based)
- **Memory footprint**: 32 bytes per header
- **Cache-friendly**: Header fits in single cache line (64 bytes)

## Implementation Status

- [x] **Session 2.1**: Binary Protocol Foundation ✓
  - [x] Fixed 32-byte header structure
  - [x] Network byte order serialization
  - [x] CRC32 checksum (IEEE 802.3)
  - [x] Message validation
  - [x] Comprehensive tests
  
- [ ] **Session 2.2**: TLV Encoder/Decoder
- [ ] **Session 2.3**: Message Definitions
- [ ] **Session 2.4**: RPC Framework
- [ ] **Session 2.5**: Phase 2 Integration

**Phase 2 Progress**: 20% (1/5 sessions complete)

## Next Steps

Session 2.2 will implement Type-Length-Value (TLV) encoding for flexible payload serialization.

## License

TBD