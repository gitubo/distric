# distric_protocol API Reference

Version: 1.0.0  
Header: `#include <distric_protocol.h>`

---

## Constants

```c
#define PROTOCOL_MAGIC       0xD15C0000
#define PROTOCOL_VERSION     1
#define MESSAGE_HEADER_SIZE  32    /* bytes */
#define MAX_PAYLOAD_LEN      (16 * 1024 * 1024)  /* 16 MiB */
```

---

## Error Codes

All functions return `distric_err_t`.

| Code | Meaning |
|------|---------|
| `DISTRIC_OK` | Success |
| `DISTRIC_ERR_INVALID_ARG` | NULL or out-of-range argument |
| `DISTRIC_ERR_NO_MEMORY` | Allocation failure |
| `DISTRIC_ERR_INVALID_FORMAT` | Malformed binary data |
| `DISTRIC_ERR_TYPE_MISMATCH` | TLV field type does not match accessor |
| `DISTRIC_ERR_NOT_FOUND` | Tag not present in TLV stream |
| `DISTRIC_ERR_EOF` | TLV decoder reached end of buffer |

---

## Binary Protocol

### Types

```c
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t msg_type;
    uint16_t flags;
    uint16_t reserved;
    uint32_t payload_len;
    uint64_t message_id;
    uint32_t timestamp_us;
    uint32_t crc32;
} message_header_t;

typedef enum {
    MSG_RAFT_REQUEST_VOTE      = 0x1001,
    MSG_RAFT_APPEND_ENTRIES    = 0x1003,
    MSG_RAFT_INSTALL_SNAPSHOT  = 0x1005,
    MSG_GOSSIP_PING            = 0x2001,
    MSG_GOSSIP_ACK             = 0x2002,
    MSG_GOSSIP_MEMBERSHIP_UPDATE = 0x2004,
    MSG_TASK_ASSIGNMENT        = 0x3001,
    MSG_TASK_RESULT            = 0x3002,
    MSG_TASK_STATUS            = 0x3003,
    MSG_CLIENT_SUBMIT          = 0x4001,
    MSG_CLIENT_RESPONSE        = 0x4002,
} message_type_t;

typedef enum {
    MSG_FLAG_NONE        = 0x0000,
    MSG_FLAG_COMPRESSED  = 0x0001,
    MSG_FLAG_ENCRYPTED   = 0x0002,
    MSG_FLAG_URGENT      = 0x0004,
    MSG_FLAG_RETRY       = 0x0008,
    MSG_FLAG_RESPONSE    = 0x0010,
} message_flag_t;
```

---

### `message_header_init`

```c
distric_err_t message_header_init(
    message_header_t* header,
    message_type_t    msg_type,
    uint32_t          payload_len
);
```

Initialise a header with magic, protocol version, a generated unique message ID, and current timestamp. `crc32` is set to zero; call `compute_header_crc32` after constructing the payload.

**Parameters**
- `header` — output struct; must not be NULL.
- `msg_type` — one of the `message_type_t` values.
- `payload_len` — byte length of the payload that will accompany this header.

**Returns** `DISTRIC_OK` on success.

---

### `serialize_header`

```c
distric_err_t serialize_header(
    const message_header_t* header,
    uint8_t*                buf
);
```

Write the header to `buf` in network byte order. `buf` must be at least `MESSAGE_HEADER_SIZE` (32) bytes.

**Returns** `DISTRIC_OK` on success.

---

### `deserialize_header`

```c
distric_err_t deserialize_header(
    const uint8_t*    buf,
    message_header_t* header
);
```

Read the header from `buf` (network byte order) into the host-order struct. `buf` must be at least `MESSAGE_HEADER_SIZE` bytes.

**Returns** `DISTRIC_OK` on success.

---

### `validate_message_header`

```c
bool validate_message_header(const message_header_t* header);
```

Return `true` if the header passes all sanity checks:

- magic equals `PROTOCOL_MAGIC`
- version equals `PROTOCOL_VERSION`
- `msg_type` is a known value
- `payload_len` does not exceed `MAX_PAYLOAD_LEN`

---

### `compute_header_crc32`

```c
distric_err_t compute_header_crc32(
    message_header_t* header,
    const uint8_t*    payload,
    size_t            payload_len
);
```

Compute CRC32 (IEEE 802.3) over the serialised header fields plus the payload bytes, and store the result in `header->crc32`. Must be called after the payload is finalised and before `serialize_header`.

---

### `verify_message_crc32`

```c
bool verify_message_crc32(
    const message_header_t* header,
    const uint8_t*          payload,
    size_t                  payload_len
);
```

Recompute CRC32 and compare against `header->crc32`. Return `true` if they match.

---

### `distric_protocol_version`

```c
static inline const char* distric_protocol_version(void);
```

Return the library version string, e.g. `"1.0.0"`.

---

## TLV Encoding

### Types

```c
typedef struct tlv_encoder tlv_encoder_t;
typedef struct tlv_decoder tlv_decoder_t;

typedef enum {
    TLV_UINT8  = 0x01,
    TLV_UINT16 = 0x02,
    TLV_UINT32 = 0x03,
    TLV_UINT64 = 0x04,
    TLV_INT32  = 0x05,
    TLV_INT64  = 0x06,
    TLV_BYTES  = 0x07,
    TLV_STRING = 0x08,
} tlv_type_t;

typedef struct {
    tlv_type_t     type;
    uint16_t       tag;
    uint32_t       length;
    const uint8_t* value;   /* points into the original buffer (zero-copy) */
} tlv_field_t;
```

---

### Encoder

#### `tlv_encoder_create`

```c
tlv_encoder_t* tlv_encoder_create(size_t initial_capacity);
```

Allocate a TLV encoder with the given initial buffer capacity. The buffer grows automatically.

**Returns** pointer on success, NULL on allocation failure.

---

#### `tlv_encode_uint8` / `uint16` / `uint32` / `uint64`

```c
distric_err_t tlv_encode_uint8 (tlv_encoder_t* enc, uint16_t tag, uint8_t  value);
distric_err_t tlv_encode_uint16(tlv_encoder_t* enc, uint16_t tag, uint16_t value);
distric_err_t tlv_encode_uint32(tlv_encoder_t* enc, uint16_t tag, uint32_t value);
distric_err_t tlv_encode_uint64(tlv_encoder_t* enc, uint16_t tag, uint64_t value);
```

Append an unsigned integer field. Values are written in network byte order.

---

#### `tlv_encode_int32` / `int64`

```c
distric_err_t tlv_encode_int32(tlv_encoder_t* enc, uint16_t tag, int32_t value);
distric_err_t tlv_encode_int64(tlv_encoder_t* enc, uint16_t tag, int64_t value);
```

Append a signed integer field.

---

#### `tlv_encode_string`

```c
distric_err_t tlv_encode_string(tlv_encoder_t* enc, uint16_t tag, const char* value);
```

Append a null-terminated string field. The null terminator is not included in the wire length.

---

#### `tlv_encode_bytes`

```c
distric_err_t tlv_encode_bytes(
    tlv_encoder_t* enc,
    uint16_t       tag,
    const uint8_t* data,
    size_t         len
);
```

Append a raw byte array field.

---

#### `tlv_encoder_finalize`

```c
uint8_t* tlv_encoder_finalize(tlv_encoder_t* enc, size_t* len_out);
```

Return a pointer to the encoded buffer and write its length to `*len_out`. The returned pointer is valid until `tlv_encoder_free` is called. Do not free it directly.

---

#### `tlv_encoder_detach`

```c
uint8_t* tlv_encoder_detach(tlv_encoder_t* enc, size_t* len_out);
```

Transfer ownership of the buffer to the caller. The encoder is left with no internal buffer. Caller must `free()` the returned pointer.

---

#### `tlv_encoder_free`

```c
void tlv_encoder_free(tlv_encoder_t* enc);
```

Free the encoder and its internal buffer (if not already detached).

---

### Decoder

#### `tlv_decoder_create`

```c
tlv_decoder_t* tlv_decoder_create(const uint8_t* buffer, size_t len);
```

Create a decoder over an existing buffer. The buffer must remain valid for the decoder's lifetime. Zero-copy: `tlv_field_t.value` points directly into `buffer`.

---

#### `tlv_decoder_free`

```c
void tlv_decoder_free(tlv_decoder_t* dec);
```

Free the decoder. Does not free the underlying buffer.

---

#### `tlv_decode_next`

```c
distric_err_t tlv_decode_next(tlv_decoder_t* dec, tlv_field_t* field_out);
```

Decode the next field from the current position.

**Returns** `DISTRIC_OK` if a field was decoded, `DISTRIC_ERR_EOF` at end of buffer, `DISTRIC_ERR_INVALID_FORMAT` on malformed data.

---

#### `tlv_find_field`

```c
distric_err_t tlv_find_field(tlv_decoder_t* dec, uint16_t tag, tlv_field_t* field_out);
```

Linear search for the first field with the given tag, starting from the current position.

**Returns** `DISTRIC_OK` if found, `DISTRIC_ERR_NOT_FOUND` if the tag is absent.

---

#### `tlv_skip_field`

```c
distric_err_t tlv_skip_field(tlv_decoder_t* dec);
```

Advance past the current field without decoding its value.

---

#### `tlv_decoder_has_more`

```c
bool tlv_decoder_has_more(const tlv_decoder_t* dec);
```

Return `true` if there are unread bytes remaining.

---

#### `tlv_decoder_reset`

```c
void tlv_decoder_reset(tlv_decoder_t* dec);
```

Reset the read position to the beginning of the buffer.

---

### Field Value Extraction

All extractors perform type and length validation and return `DISTRIC_ERR_TYPE_MISMATCH` on type mismatch.

```c
distric_err_t tlv_field_get_uint8 (const tlv_field_t* f, uint8_t*  out);
distric_err_t tlv_field_get_uint16(const tlv_field_t* f, uint16_t* out);
distric_err_t tlv_field_get_uint32(const tlv_field_t* f, uint32_t* out);
distric_err_t tlv_field_get_uint64(const tlv_field_t* f, uint64_t* out);
distric_err_t tlv_field_get_int32 (const tlv_field_t* f, int32_t*  out);
distric_err_t tlv_field_get_int64 (const tlv_field_t* f, int64_t*  out);

/* Returns a pointer into the decoder's buffer — zero-copy, do not free. */
const char*   tlv_field_get_string(const tlv_field_t* f);

distric_err_t tlv_field_get_bytes(
    const tlv_field_t* f,
    const uint8_t**    data_out,
    size_t*            len_out
);
```

---

### `tlv_validate_buffer`

```c
bool tlv_validate_buffer(const uint8_t* buffer, size_t len);
```

Scan the entire buffer and verify that all TLV fields are well-formed. Use before decoding untrusted data.

---

## Protocol Messages

### Raft Messages

```c
/* Structures */
typedef struct {
    uint32_t term;
    char     candidate_id[64];
    uint32_t last_log_index;
    uint32_t last_log_term;
} raft_request_vote_t;

typedef struct {
    uint32_t term;
    bool     vote_granted;
} raft_request_vote_response_t;

typedef struct {
    uint32_t term;
    char     leader_id[64];
    uint32_t prev_log_index;
    uint32_t prev_log_term;
    uint8_t* entries;
    size_t   entries_len;
    uint32_t leader_commit;
} raft_append_entries_t;

/* Serialisation */
distric_err_t serialize_raft_request_vote(
    const raft_request_vote_t* msg,
    uint8_t** buffer_out, size_t* len_out);

distric_err_t deserialize_raft_request_vote(
    const uint8_t* buffer, size_t len,
    raft_request_vote_t* msg_out);

void free_raft_request_vote(raft_request_vote_t* msg);

distric_err_t serialize_raft_request_vote_response(
    const raft_request_vote_response_t* msg,
    uint8_t** buffer_out, size_t* len_out);

distric_err_t deserialize_raft_request_vote_response(
    const uint8_t* buffer, size_t len,
    raft_request_vote_response_t* msg_out);

distric_err_t serialize_raft_append_entries(
    const raft_append_entries_t* msg,
    uint8_t** buffer_out, size_t* len_out);

distric_err_t deserialize_raft_append_entries(
    const uint8_t* buffer, size_t len,
    raft_append_entries_t* msg_out);

void free_raft_append_entries(raft_append_entries_t* msg);
```

---

### Gossip Messages

```c
typedef struct {
    char     sender_id[64];
    uint64_t timestamp;
    uint32_t sequence;
} gossip_ping_t;

typedef struct {
    char     sender_id[64];
    uint32_t sequence;
} gossip_ack_t;

distric_err_t serialize_gossip_ping(
    const gossip_ping_t* msg, uint8_t** out, size_t* len);
distric_err_t deserialize_gossip_ping(
    const uint8_t* buf, size_t len, gossip_ping_t* out);

distric_err_t serialize_gossip_ack(
    const gossip_ack_t* msg, uint8_t** out, size_t* len);
distric_err_t deserialize_gossip_ack(
    const uint8_t* buf, size_t len, gossip_ack_t* out);
```

---

### Task Messages

```c
typedef struct {
    char     task_id[128];
    char     workflow_id[128];
    char     task_type[64];
    char*    config_json;
    uint8_t* input_data;
    size_t   input_data_len;
    uint32_t timeout_sec;
    uint32_t retry_count;
} task_assignment_t;

typedef struct {
    char     task_id[128];
    char     worker_id[64];
    uint32_t status;
    uint8_t* output_data;
    size_t   output_data_len;
    char*    error_message;
    int32_t  exit_code;
    uint64_t started_at;
    uint64_t completed_at;
} task_result_t;

distric_err_t serialize_task_assignment(
    const task_assignment_t* msg, uint8_t** out, size_t* len);
distric_err_t deserialize_task_assignment(
    const uint8_t* buf, size_t len, task_assignment_t* out);
void free_task_assignment(task_assignment_t* msg);

distric_err_t serialize_task_result(
    const task_result_t* msg, uint8_t** out, size_t* len);
distric_err_t deserialize_task_result(
    const uint8_t* buf, size_t len, task_result_t* out);
void free_task_result(task_result_t* msg);
```

---

### Client Messages

```c
typedef struct {
    char     message_id[128];
    char     event_type[64];
    char*    payload_json;
    uint64_t timestamp;
} client_submit_t;

typedef struct {
    char    message_id[128];
    uint32_t response_code;
    char*   response_message;
    char**  workflows_triggered;
    size_t  workflow_count;
} client_response_t;

distric_err_t serialize_client_submit(
    const client_submit_t* msg, uint8_t** out, size_t* len);
distric_err_t deserialize_client_submit(
    const uint8_t* buf, size_t len, client_submit_t* out);
void free_client_submit(client_submit_t* msg);

distric_err_t serialize_client_response(
    const client_response_t* msg, uint8_t** out, size_t* len);
distric_err_t deserialize_client_response(
    const uint8_t* buf, size_t len, client_response_t* out);
void free_client_response(client_response_t* msg);
```

**Memory** — all `deserialize_*` functions that contain pointer fields (`char*`, `uint8_t*`) allocate heap memory. Always call the corresponding `free_*` function when done.

---

## Typical Encoding Workflow

```
tlv_encoder_create
  -> tlv_encode_* (one per field)
  -> tlv_encoder_finalize
message_header_init
compute_header_crc32
serialize_header
send(header) + send(payload)
tlv_encoder_free
```

## Typical Decoding Workflow

```
recv(header bytes)
deserialize_header
validate_message_header -> reject if false
recv(payload bytes)
verify_message_crc32 -> reject if false
tlv_decoder_create
  -> tlv_decode_next in loop
  -> tlv_field_get_* per field
tlv_decoder_free
free(payload)
```
