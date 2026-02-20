# distric_protocol

Binary wire protocol library for the DistriC platform. Provides a fixed 32-byte message header, TLV payload encoding, CRC32 integrity checking, and complete serialisation/deserialisation for all DistriC message types.

## Overview

`distric_protocol` defines and implements the on-wire format used by all DistriC nodes. Every message consists of a fixed 32-byte header in network byte order followed by a variable-length payload encoded in Type-Length-Value (TLV) format. CRC32 covers both header and payload, providing corruption detection at the transport boundary.

## Features

- Fixed 32-byte header with magic number, version, message type, and CRC32
- Network byte order (big-endian) throughout
- TLV payload encoding with dynamic buffer growth and zero-copy decoding
- Forward-compatibility: unknown TLV fields are safely skipped
- CRC32 (IEEE 802.3) for corruption detection
- Complete message definitions for Raft, Gossip, Task, and Client protocols
- No external dependencies beyond `distric_obs`

## Requirements

- C11 compiler with GNU extensions
- CMake 3.15 or later
- `distric_obs` library

## Build

```bash
cmake -B build -DBUILD_TESTING=ON
cmake --build build
cd build && ctest -R test_binary --output-on-failure
```

## Wire Format

### Header Layout (32 bytes)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           Magic (0xD15C0000)                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Version              |         Message Type          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|            Flags              |           Reserved            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Payload Length                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       Message ID (high)                       |
|                       Message ID (low)                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Timestamp µs                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                            CRC32                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

Total: 32 bytes, packed, network byte order.

### TLV Field Layout

```
Byte 0    : Type  (uint8_t)
Bytes 1-2 : Tag   (uint16_t, big-endian)
Bytes 3-6 : Length (uint32_t, big-endian)
Bytes 7+  : Value  (N bytes)
```

## Message Types

### Raft Consensus (0x1xxx)

| Constant | Value | Description |
|----------|-------|-------------|
| `MSG_RAFT_REQUEST_VOTE` | 0x1001 | Leader election vote request |
| `MSG_RAFT_APPEND_ENTRIES` | 0x1003 | Log replication / heartbeat |
| `MSG_RAFT_INSTALL_SNAPSHOT` | 0x1005 | Snapshot transfer |

### Gossip Protocol (0x2xxx)

| Constant | Value | Description |
|----------|-------|-------------|
| `MSG_GOSSIP_PING` | 0x2001 | Failure detection probe |
| `MSG_GOSSIP_ACK` | 0x2002 | Probe acknowledgement |
| `MSG_GOSSIP_MEMBERSHIP_UPDATE` | 0x2004 | Membership state push |

### Task Execution (0x3xxx)

| Constant | Value | Description |
|----------|-------|-------------|
| `MSG_TASK_ASSIGNMENT` | 0x3001 | Assign task to worker |
| `MSG_TASK_RESULT` | 0x3002 | Worker result delivery |
| `MSG_TASK_STATUS` | 0x3003 | In-progress status update |

### Client API (0x4xxx)

| Constant | Value | Description |
|----------|-------|-------------|
| `MSG_CLIENT_SUBMIT` | 0x4001 | Submit event for processing |
| `MSG_CLIENT_RESPONSE` | 0x4002 | Processing acknowledgement |

## Message Flags

```c
MSG_FLAG_NONE        /* No flags set               */
MSG_FLAG_COMPRESSED  /* Payload is compressed       */
MSG_FLAG_ENCRYPTED   /* Payload is encrypted        */
MSG_FLAG_URGENT      /* High-priority routing hint  */
MSG_FLAG_RETRY       /* This is a retransmission    */
MSG_FLAG_RESPONSE    /* This is a response message  */
```

## Quick Start

### Encoding

```c
#include <distric_protocol.h>

/* 1. Build TLV payload */
tlv_encoder_t* enc = tlv_encoder_create(256);
tlv_encode_uint32(enc, FIELD_TERM, 42);
tlv_encode_string(enc, FIELD_CANDIDATE_ID, "node-123");

size_t   payload_len;
uint8_t* payload = tlv_encoder_finalize(enc, &payload_len);

/* 2. Initialise header */
message_header_t header;
message_header_init(&header, MSG_RAFT_REQUEST_VOTE, (uint32_t)payload_len);

/* 3. Compute and embed CRC32 */
compute_header_crc32(&header, payload, payload_len);

/* 4. Serialise header to wire bytes */
uint8_t header_buf[MESSAGE_HEADER_SIZE];
serialize_header(&header, header_buf);

/* 5. Transmit */
send(sock, header_buf, MESSAGE_HEADER_SIZE, 0);
send(sock, payload,    payload_len,          0);

tlv_encoder_free(enc);
```

### Decoding

```c
/* 1. Receive fixed header */
uint8_t header_buf[MESSAGE_HEADER_SIZE];
recv(sock, header_buf, MESSAGE_HEADER_SIZE, 0);

/* 2. Deserialise and validate */
message_header_t header;
deserialize_header(header_buf, &header);

if (!validate_message_header(&header)) { /* reject */ }

/* 3. Receive payload */
uint8_t* payload = malloc(header.payload_len);
recv(sock, payload, header.payload_len, 0);

/* 4. Verify integrity */
if (!verify_message_crc32(&header, payload, header.payload_len)) {
    free(payload);
    /* reject */
}

/* 5. Decode TLV fields */
tlv_decoder_t* dec = tlv_decoder_create(payload, header.payload_len);
tlv_field_t field;
while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
    switch (field.tag) {
        case FIELD_TERM: {
            uint32_t term;
            tlv_field_get_uint32(&field, &term);
            break;
        }
        case FIELD_CANDIDATE_ID: {
            const char* id = tlv_field_get_string(&field);
            break;
        }
    }
}
tlv_decoder_free(dec);
free(payload);
```

## Performance Characteristics

| Operation | Latency |
|-----------|---------|
| Header serialisation | ~50–100 ns |
| Header deserialisation | ~50–100 ns |
| CRC32 computation | ~5–10 cycles/byte |
| Header memory footprint | 32 bytes (fits single cache line) |

## Directory Structure

```
libs/distric_protocol/
├── CMakeLists.txt
├── README.md
├── API.md
├── include/
│   ├── distric_protocol.h          # Single public header (use this)
│   └── distric_protocol/           # Internal sub-headers
│       ├── binary.h
│       ├── crc32.h
│       ├── tlv.h
│       ├── messages.h
│       └── rpc.h
└── src/
    ├── binary.c
    ├── crc32.c
    ├── tlv.c
    ├── messages.c
    └── rpc.c
```

## API Stability

`include/distric_protocol.h` is the only stable public interface. Sub-headers under `include/distric_protocol/` are internal and must not be included directly.

## License

See repository root for license information.
