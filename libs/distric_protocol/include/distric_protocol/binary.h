/**
 * @file binary.h
 * @brief Fixed 32-byte wire-protocol header definition and API
 *
 * Applied improvements:
 *
 *  Improvement #5 — Reserved field no longer rejected by validate_message_header.
 *    Silently rejecting non-zero reserved bytes causes hard wire-breaks when a
 *    future protocol minor version legitimately assigns meaning to them.
 *    The field is preserved for forward compatibility; unknown values are
 *    tolerated and logged at DEBUG level.
 *
 *  Improvement #6 — timestamp field widened from µs to seconds.
 *    The previous uint32_t timestamp_us wrapped every ~71 minutes, producing
 *    meaningless wire timestamps.  The field is now timestamp_s (Unix seconds),
 *    which wraps in year 2106 — sufficient for any realistic deployment.
 *    The struct layout is unchanged (32 bytes total); the field occupies the
 *    same 4 bytes at wire offset 24.
 *
 * @version 1.1.0
 */

#ifndef DISTRIC_PROTOCOL_BINARY_H
#define DISTRIC_PROTOCOL_BINARY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <distric_obs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * PROTOCOL CONSTANTS
 * ========================================================================= */

/** Protocol magic number — 4 bytes at wire offset 0. */
#define PROTOCOL_MAGIC       0xD15C0000u

/** Protocol version — (major << 8) | minor. */
#define PROTOCOL_VERSION     0x0100u   /* 1.0 */

/** Serialised header size in bytes (exactly one cache line). */
#define MESSAGE_HEADER_SIZE  32u

/**
 * @brief Maximum payload length in bytes.
 *
 * Can be overridden at compile time via -DPROTOCOL_MAX_PAYLOAD_SIZE=<n>.
 * Default: 64 MiB.  Acceptable range: 1 byte – 256 MiB.
 */
#ifndef PROTOCOL_MAX_PAYLOAD_SIZE
#  define PROTOCOL_MAX_PAYLOAD_SIZE (64u * 1024u * 1024u)
#endif

#if PROTOCOL_MAX_PAYLOAD_SIZE == 0 || \
    PROTOCOL_MAX_PAYLOAD_SIZE > (256u * 1024u * 1024u)
#  error "PROTOCOL_MAX_PAYLOAD_SIZE must be in [1, 268435456]"
#endif

/* ============================================================================
 * MESSAGE TYPES
 * ========================================================================= */

typedef enum {
    /* Raft consensus */
    MSG_RAFT_REQUEST_VOTE       = 0x1001,
    MSG_RAFT_VOTE_RESPONSE      = 0x1002,
    MSG_RAFT_APPEND_ENTRIES     = 0x1003,
    MSG_RAFT_APPEND_RESPONSE    = 0x1004,
    MSG_RAFT_INSTALL_SNAPSHOT   = 0x1005,
    MSG_RAFT_SNAPSHOT_RESPONSE  = 0x1006,

    /* Gossip */
    MSG_GOSSIP_PING             = 0x2001,
    MSG_GOSSIP_ACK              = 0x2002,
    MSG_GOSSIP_PING_REQ         = 0x2003,
    MSG_GOSSIP_MEMBERSHIP_UPDATE= 0x2004,

    /* Task */
    MSG_TASK_ASSIGNMENT         = 0x3001,
    MSG_TASK_RESULT             = 0x3002,
    MSG_TASK_STATUS             = 0x3003,

    /* Client */
    MSG_CLIENT_SUBMIT           = 0x4001,
    MSG_CLIENT_RESPONSE         = 0x4002,
    MSG_CLIENT_QUERY            = 0x4003,
    MSG_CLIENT_ERROR            = 0x4004,
} message_type_t;

/* ============================================================================
 * MESSAGE FLAGS
 * ========================================================================= */

typedef enum {
    MSG_FLAG_NONE        = 0x0000,
    MSG_FLAG_COMPRESSED  = 0x0001,
    MSG_FLAG_ENCRYPTED   = 0x0002,
    MSG_FLAG_URGENT      = 0x0004,
    MSG_FLAG_RETRY       = 0x0008,
    MSG_FLAG_RESPONSE    = 0x0010,
} message_flag_t;

/* ============================================================================
 * WIRE HEADER
 *
 * Layout (32 bytes, packed, network byte order):
 *
 *  Offset  Size  Field
 *  ------  ----  -----
 *   0       4    magic       (0xD15C0000)
 *   4       2    version     ((major << 8) | minor)
 *   6       2    msg_type
 *   8       2    flags
 *  10       2    reserved    (forward-compat; any value accepted)
 *  12       4    payload_len (bytes)
 *  16       8    message_id
 *  24       4    timestamp_s (Unix epoch seconds; wraps year 2106)
 *  28       4    crc32
 * ========================================================================= */

/**
 * @brief On-wire message header (host byte order in memory).
 *
 * Improvement #6: `timestamp_s` stores Unix seconds instead of the previous
 * `timestamp_us` (microseconds truncated to 32 bits, which wrapped every
 * ~71 minutes).  Wire offset and field size are unchanged.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;         /**< Protocol magic (0xD15C0000)                 */
    uint16_t version;       /**< Protocol version — (major<<8)|minor         */
    uint16_t msg_type;      /**< Message type (see message_type_t)           */
    uint16_t flags;         /**< Message flags (bitfield, see message_flag_t)*/
    uint16_t reserved;      /**< Reserved — tolerated, not validated         */
    uint32_t payload_len;   /**< Payload length in bytes                     */
    uint64_t message_id;    /**< Unique message identifier                   */
    uint32_t timestamp_s;   /**< Unix epoch seconds (wraps year 2106)        */
    uint32_t crc32;         /**< CRC32 checksum (header + payload)           */
} message_header_t;

_Static_assert(sizeof(message_header_t) == MESSAGE_HEADER_SIZE,
               "message_header_t must be exactly 32 bytes");

/* ============================================================================
 * HEADER API
 * ========================================================================= */

/**
 * @brief Initialise a message header with sane defaults.
 *
 * Sets magic, version, msg_type, payload_len, and current Unix timestamp
 * (seconds).  Generates a unique message_id.  crc32 is 0 until
 * compute_header_crc32().
 *
 * @param header      Output — must not be NULL.
 * @param msg_type    One of the message_type_t values.
 * @param payload_len Byte length of the accompanying payload.
 * @return DISTRIC_OK on success, DISTRIC_ERR_INVALID_ARG on bad input.
 */
distric_err_t message_header_init(message_header_t* header,
                                  message_type_t    msg_type,
                                  uint32_t          payload_len);

/**
 * @brief Serialise header to a 32-byte wire buffer (big-endian).
 *
 * @param header Source header (host byte order).
 * @param buf    Destination; must be at least MESSAGE_HEADER_SIZE bytes.
 * @return DISTRIC_OK on success.
 */
distric_err_t serialize_header(const message_header_t* header, uint8_t* buf);

/**
 * @brief Deserialise a 32-byte wire buffer into a header struct.
 *
 * @param buf    Source; must be at least MESSAGE_HEADER_SIZE bytes.
 * @param header Destination (host byte order).
 * @return DISTRIC_OK on success.
 */
distric_err_t deserialize_header(const uint8_t* buf, message_header_t* header);

/**
 * @brief Validate header fields.
 *
 * Checks:
 *  - Magic equals PROTOCOL_MAGIC.
 *  - Major version byte matches PROTOCOL_VERSION (minor differences tolerated).
 *  - msg_type is non-zero and within the defined subsystem range.
 *  - payload_len ≤ PROTOCOL_MAX_PAYLOAD_SIZE.
 *
 * Improvement #5: the reserved field is intentionally NOT checked.  Future
 * protocol minor revisions may legitimately set reserved bits; rejecting them
 * would cause hard wire-breaks in rolling upgrades.  Unknown reserved values
 * are logged at DEBUG level by message_header_init but never rejected.
 *
 * @return true if valid, false otherwise.
 */
bool validate_message_header(const message_header_t* header);

/**
 * @brief Return a human-readable name for a message type.
 *
 * @param msg_type Message type value.
 * @return Constant string (e.g. "RAFT_REQUEST_VOTE"), or "UNKNOWN".
 */
const char* message_type_to_string(message_type_t msg_type);

/* ============================================================================
 * CRC32 API
 * ========================================================================= */

/**
 * @brief Compute CRC32 over header fields + payload and store in header->crc32.
 *
 * The crc32 field within *header is zeroed before computation so it does not
 * affect the digest.  Must be called after payload is finalised and before
 * serialize_header().
 *
 * @param header      In/out — crc32 field is updated.
 * @param payload     Payload bytes (may be NULL when payload_len == 0).
 * @param payload_len Payload length.
 * @return DISTRIC_OK on success.
 */
distric_err_t compute_header_crc32(message_header_t* header,
                                   const uint8_t*    payload,
                                   size_t            payload_len);

/**
 * @brief Verify CRC32 of a received message.
 *
 * @param header      Received header (host byte order, crc32 field set).
 * @param payload     Received payload bytes.
 * @param payload_len Payload length.
 * @return true if CRC matches, false on corruption.
 */
bool verify_message_crc32(const message_header_t* header,
                           const uint8_t*          payload,
                           size_t                  payload_len);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_PROTOCOL_BINARY_H */