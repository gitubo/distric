/**
 * @file binary.h
 * @brief Binary Protocol Header Definitions
 *
 * Fixed 32-byte message header with network byte order, CRC32, and
 * version negotiation.
 *
 * Header Layout (32 bytes):
 * ┌─────────────────────────────────────────┐
 * │  Offset | Size | Field                  │
 * ├─────────┼──────┼────────────────────────┤
 * │  0      | 4    | Magic (0x44495354)     │
 * │  4      | 2    | Version (0x0001)       │
 * │  6      | 2    | Message Type           │
 * │  8      | 2    | Flags                  │
 * │  10     | 2    | Reserved               │
 * │  12     | 4    | Payload Length         │
 * │  16     | 8    | Message ID             │
 * │  24     | 4    | Timestamp (microsec)   │
 * │  28     | 4    | CRC32                  │
 * └─────────────────────────────────────────┘
 *
 * Version negotiation (item 7):
 *  - Major version mismatch (high byte) → reject connection.
 *  - Minor version difference (low byte) → accept with degraded features.
 *  - validate_message_header() enforces this rule.
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

/** Protocol magic number: "DIST" in ASCII */
#define PROTOCOL_MAGIC   0x44495354u

/** Current protocol version (major=0x00, minor=0x01) */
#define PROTOCOL_VERSION 0x0001u

/** Fixed message header size (32 bytes) */
#define MESSAGE_HEADER_SIZE 32u

/**
 * @brief Absolute maximum payload length accepted from the wire.
 *
 * validate_message_header() rejects any header whose payload_len field
 * exceeds this value — protecting against trivially large allocations
 * before any application-level size checks.
 *
 * This is the global floor; the RPC layer enforces its own (possibly smaller)
 * limit via RPC_MAX_MESSAGE_SIZE.
 *
 * Override at compile time with -DPROTOCOL_MAX_PAYLOAD_SIZE=<bytes>.
 * Default: 64 MiB.
 */
#ifndef PROTOCOL_MAX_PAYLOAD_SIZE
#define PROTOCOL_MAX_PAYLOAD_SIZE (64u * 1024u * 1024u)
#endif

/* ============================================================================
 * MESSAGE TYPES
 * ========================================================================= */

typedef enum {
    /* Raft consensus messages (0x1000-0x1FFF) */
    MSG_RAFT_REQUEST_VOTE               = 0x1001,
    MSG_RAFT_REQUEST_VOTE_RESPONSE      = 0x1002,
    MSG_RAFT_APPEND_ENTRIES             = 0x1003,
    MSG_RAFT_APPEND_ENTRIES_RESPONSE    = 0x1004,
    MSG_RAFT_INSTALL_SNAPSHOT           = 0x1005,
    MSG_RAFT_INSTALL_SNAPSHOT_RESPONSE  = 0x1006,

    /* Gossip protocol messages (0x2000-0x2FFF) */
    MSG_GOSSIP_PING                     = 0x2001,
    MSG_GOSSIP_ACK                      = 0x2002,
    MSG_GOSSIP_INDIRECT_PING            = 0x2003,
    MSG_GOSSIP_MEMBERSHIP_UPDATE        = 0x2004,
    MSG_GOSSIP_SUSPECT                  = 0x2005,
    MSG_GOSSIP_ALIVE                    = 0x2006,

    /* Task execution messages (0x3000-0x3FFF) */
    MSG_TASK_ASSIGNMENT                 = 0x3001,
    MSG_TASK_RESULT                     = 0x3002,
    MSG_TASK_STATUS                     = 0x3003,
    MSG_TASK_CANCEL                     = 0x3004,
    MSG_TASK_HEARTBEAT                  = 0x3005,

    /* Client API messages (0x4000-0x4FFF) */
    MSG_CLIENT_SUBMIT                   = 0x4001,
    MSG_CLIENT_RESPONSE                 = 0x4002,
    MSG_CLIENT_QUERY                    = 0x4003,
    MSG_CLIENT_ERROR                    = 0x4004,
} message_type_t;

/* ============================================================================
 * MESSAGE FLAGS
 * ========================================================================= */

#define MSG_FLAG_NONE        0x0000u  /**< No flags set                    */
#define MSG_FLAG_COMPRESSED  0x0001u  /**< Payload is compressed           */
#define MSG_FLAG_ENCRYPTED   0x0002u  /**< Payload is encrypted            */
#define MSG_FLAG_URGENT      0x0004u  /**< High-priority message           */
#define MSG_FLAG_RETRY       0x0008u  /**< This is a retry of a prior msg  */
#define MSG_FLAG_RESPONSE    0x0010u  /**< This is a response message      */

/* ============================================================================
 * MESSAGE HEADER STRUCTURE
 * ========================================================================= */

/**
 * @brief Binary message header (32 bytes, packed).
 *
 * All multi-byte fields are in network byte order (big-endian).
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;         /**< Protocol magic (0x44495354 = "DIST")       */
    uint16_t version;       /**< Protocol version — see PROTOCOL_VERSION     */
    uint16_t msg_type;      /**< Message type (see message_type_t)           */
    uint16_t flags;         /**< Message flags (bitfield)                    */
    uint16_t reserved;      /**< Reserved for future use (must be 0)        */
    uint32_t payload_len;   /**< Payload length in bytes                     */
    uint64_t message_id;    /**< Unique message identifier                   */
    uint32_t timestamp_us;  /**< Timestamp in microseconds (lower 32 bits)  */
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
 * Sets magic, version, msg_type, payload_len, and timestamp.
 * Generates a unique message_id.  CRC32 is 0 until compute_header_crc32().
 */
distric_err_t message_header_init(message_header_t* header,
                                  message_type_t    msg_type,
                                  uint32_t          payload_len);

/**
 * @brief Serialise header to 32-byte wire buffer (big-endian).
 */
distric_err_t serialize_header(const message_header_t* header, uint8_t* buf);

/**
 * @brief Deserialise 32-byte wire buffer to header struct.
 */
distric_err_t deserialize_header(const uint8_t* buf, message_header_t* header);

/**
 * @brief Validate header fields.
 *
 * Checks:
 *  - Magic number matches PROTOCOL_MAGIC.
 *  - Major version byte matches PROTOCOL_VERSION (minor is tolerated).
 *  - payload_len ≤ PROTOCOL_MAX_PAYLOAD_SIZE.
 *  - reserved field is zero.
 *
 * @return true if valid, false otherwise.
 */
bool validate_message_header(const message_header_t* header);

/**
 * @brief Return a human-readable name for a message type.
 */
const char* message_type_to_string(message_type_t msg_type);

/* ============================================================================
 * CRC32 API (declared here, implemented in crc32.c)
 * ========================================================================= */

/**
 * @brief Compute CRC32 over header fields + payload and store in header.crc32.
 *
 * The crc32 field within *header is zeroed before computation.
 */
distric_err_t compute_header_crc32(message_header_t* header,
                                   const uint8_t*    payload,
                                   size_t            payload_len);

/**
 * @brief Verify CRC32 of a received message.
 *
 * @return true if the CRC matches, false on corruption.
 */
bool verify_message_crc32(const message_header_t* header,
                          const uint8_t*          payload,
                          size_t                  payload_len);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_PROTOCOL_BINARY_H */