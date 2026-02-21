#ifndef DISTRIC_PROTOCOL_BINARY_H
#define DISTRIC_PROTOCOL_BINARY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <distric_obs.h>   /* distric_err_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * PROTOCOL CONSTANTS
 * ========================================================================= */

#define PROTOCOL_MAGIC            0xD15C0000u
#define PROTOCOL_VERSION          0x0101u    /* major=1, minor=1 */
#define MESSAGE_HEADER_SIZE       32u

#ifndef PROTOCOL_MAX_PAYLOAD_SIZE
#  define PROTOCOL_MAX_PAYLOAD_SIZE (64u * 1024u * 1024u)
#endif

/* ============================================================================
 * MESSAGE TYPES
 * ========================================================================= */

typedef enum {
    /* Raft consensus — 0x1000–0x1FFF */
    MSG_RAFT_REQUEST_VOTE      = 0x1001,
    MSG_RAFT_VOTE_RESPONSE     = 0x1002,
    MSG_RAFT_APPEND_ENTRIES    = 0x1003,
    MSG_RAFT_APPEND_RESPONSE   = 0x1004,
    MSG_RAFT_INSTALL_SNAPSHOT  = 0x1005,
    MSG_RAFT_SNAPSHOT_RESPONSE = 0x1006,

    /* Gossip — 0x2000–0x2FFF */
    MSG_GOSSIP_PING              = 0x2001,
    MSG_GOSSIP_ACK               = 0x2002,
    MSG_GOSSIP_PING_REQ          = 0x2003,
    MSG_GOSSIP_MEMBERSHIP_UPDATE = 0x2004,

    /* Task execution — 0x3000–0x3FFF */
    MSG_TASK_ASSIGNMENT = 0x3001,
    MSG_TASK_RESULT     = 0x3002,
    MSG_TASK_STATUS     = 0x3003,

    /* Client API — 0x4000–0x4EFF */
    MSG_CLIENT_SUBMIT   = 0x4001,
    MSG_CLIENT_RESPONSE = 0x4002,
    MSG_CLIENT_QUERY    = 0x4003,
    MSG_CLIENT_ERROR    = 0x4004,

    /* Protocol control — 0x4F00–0x4FFF */
    MSG_CAPABILITY      = 0x4F01,
} message_type_t;

/* ============================================================================
 * MESSAGE FLAGS
 * ========================================================================= */

typedef enum {
    MSG_FLAG_NONE          = 0x0000,   /* no flags set                       */
    MSG_FLAG_COMPRESSED    = 0x0001,   /* payload is compressed              */
    MSG_FLAG_ENCRYPTED     = 0x0002,   /* payload is encrypted               */
    MSG_FLAG_URGENT        = 0x0004,   /* high-priority delivery             */
    MSG_FLAG_RETRY         = 0x0008,   /* retransmission of a previous msg   */
    MSG_FLAG_RESPONSE      = 0x0010,   /* this is a response message         */
    /* Production hardening additions (>= 0x0020) */
    MSG_FLAG_AUTHENTICATED = 0x0020,   /* 32-byte HMAC appended after payload */
    MSG_FLAG_CAPABILITY    = 0x0040,   /* capability exchange message        */
    MSG_FLAG_FRAGMENT      = 0x0080,   /* message is a chunk (not final)     */
    MSG_FLAG_LAST_FRAGMENT = 0x0100,   /* last chunk in a chunked stream     */
} message_flag_t;

/* ============================================================================
 * MESSAGE HEADER  (wire: 32 bytes, big-endian)
 *
 * [0..3]   magic        4 bytes
 * [4..5]   version      2 bytes   (major<<8)|minor
 * [6..7]   msg_type     2 bytes
 * [8..9]   flags        2 bytes
 * [10..11] reserved     2 bytes   (tolerated, not validated)
 * [12..15] payload_len  4 bytes
 * [16..23] message_id   8 bytes
 * [24..27] timestamp_s  4 bytes   (Unix epoch seconds)
 * [28..31] crc32        4 bytes
 * ========================================================================= */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t msg_type;
    uint16_t flags;
    uint16_t reserved;
    uint32_t payload_len;
    uint64_t message_id;
    uint32_t timestamp_s;
    uint32_t crc32;
} message_header_t;

_Static_assert(sizeof(message_header_t) == MESSAGE_HEADER_SIZE,
               "message_header_t must be exactly 32 bytes");

/* ============================================================================
 * HEADER API
 * ========================================================================= */

distric_err_t message_header_init(message_header_t *header,
                                   message_type_t    msg_type,
                                   uint32_t          payload_len);

distric_err_t serialize_header(const message_header_t *header, uint8_t *buf);

distric_err_t deserialize_header(const uint8_t *buf, message_header_t *header);

bool validate_message_header(const message_header_t *header);

const char *message_type_to_string(message_type_t msg_type);

/* ============================================================================
 * CRC32
 * ========================================================================= */

distric_err_t compute_header_crc32(message_header_t *header,
                                    const uint8_t    *payload,
                                    size_t            payload_len);

bool verify_message_crc32(const message_header_t *header,
                           const uint8_t          *payload,
                           size_t                  payload_len);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_PROTOCOL_BINARY_H */