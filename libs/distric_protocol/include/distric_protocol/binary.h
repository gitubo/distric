/**
 * @file binary.h
 * @brief Binary Protocol Header Definitions
 * 
 * Fixed 32-byte message header structure with network byte order.
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
 * │  24     | 4    | Timestamp (seconds)    │
 * │  28     | 4    | CRC32                  │
 * └─────────────────────────────────────────┘
 * 
 * @version 1.0.0
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
#define PROTOCOL_MAGIC 0x44495354

/** Current protocol version */
#define PROTOCOL_VERSION 0x0001

/** Fixed message header size (32 bytes) */
#define MESSAGE_HEADER_SIZE 32

/* ============================================================================
 * MESSAGE TYPES
 * ========================================================================= */

/**
 * @brief Message type enumeration
 * 
 * Message types are organized by subsystem:
 * - 0x1xxx: Raft consensus messages
 * - 0x2xxx: Gossip protocol messages
 * - 0x3xxx: Task execution messages
 * - 0x4xxx: Client API messages
 */
typedef enum {
    /* Raft consensus messages (0x1000-0x1FFF) */
    MSG_RAFT_REQUEST_VOTE           = 0x1001,
    MSG_RAFT_REQUEST_VOTE_RESPONSE  = 0x1002,
    MSG_RAFT_APPEND_ENTRIES         = 0x1003,
    MSG_RAFT_APPEND_ENTRIES_RESPONSE = 0x1004,
    MSG_RAFT_INSTALL_SNAPSHOT       = 0x1005,
    MSG_RAFT_INSTALL_SNAPSHOT_RESPONSE = 0x1006,
    
    /* Gossip protocol messages (0x2000-0x2FFF) */
    MSG_GOSSIP_PING                 = 0x2001,
    MSG_GOSSIP_ACK                  = 0x2002,
    MSG_GOSSIP_INDIRECT_PING        = 0x2003,
    MSG_GOSSIP_MEMBERSHIP_UPDATE    = 0x2004,
    MSG_GOSSIP_SUSPECT              = 0x2005,
    MSG_GOSSIP_ALIVE                = 0x2006,
    
    /* Task execution messages (0x3000-0x3FFF) */
    MSG_TASK_ASSIGNMENT             = 0x3001,
    MSG_TASK_RESULT                 = 0x3002,
    MSG_TASK_STATUS                 = 0x3003,
    MSG_TASK_CANCEL                 = 0x3004,
    MSG_TASK_HEARTBEAT              = 0x3005,
    
    /* Client API messages (0x4000-0x4FFF) */
    MSG_CLIENT_SUBMIT               = 0x4001,
    MSG_CLIENT_RESPONSE             = 0x4002,
    MSG_CLIENT_QUERY                = 0x4003,
    MSG_CLIENT_ERROR                = 0x4004,
} message_type_t;

/* ============================================================================
 * MESSAGE FLAGS
 * ========================================================================= */

/** Message flags (bitfield) */
#define MSG_FLAG_NONE           0x0000
#define MSG_FLAG_COMPRESSED     0x0001  /**< Payload is compressed */
#define MSG_FLAG_ENCRYPTED      0x0002  /**< Payload is encrypted */
#define MSG_FLAG_URGENT         0x0004  /**< High-priority message */
#define MSG_FLAG_RETRY          0x0008  /**< This is a retry */
#define MSG_FLAG_RESPONSE       0x0010  /**< This is a response */

/* ============================================================================
 * MESSAGE HEADER STRUCTURE
 * ========================================================================= */

/**
 * @brief Binary message header (32 bytes, packed)
 * 
 * All multi-byte fields are in network byte order (big-endian).
 * The header is designed for efficient parsing and cache-line alignment.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;         /**< Protocol magic (0x44495354 = "DIST") */
    uint16_t version;       /**< Protocol version (0x0001) */
    uint16_t msg_type;      /**< Message type (see message_type_t) */
    uint16_t flags;         /**< Message flags (bitfield) */
    uint16_t reserved;      /**< Reserved for future use (must be 0) */
    uint32_t payload_len;   /**< Payload length in bytes */
    uint64_t message_id;    /**< Unique message identifier */
    uint32_t timestamp;     /**< Unix timestamp (seconds since epoch) */
    uint32_t crc32;         /**< CRC32 checksum (header + payload) */
} message_header_t;

/* Compile-time assertion for header size */
_Static_assert(sizeof(message_header_t) == MESSAGE_HEADER_SIZE, 
               "message_header_t must be exactly 32 bytes");

/* ============================================================================
 * HEADER MANIPULATION API
 * ========================================================================= */

/**
 * @brief Initialize a message header with defaults
 * 
 * Sets magic, version, and generates a unique message_id.
 * Sets timestamp to current time.
 * CRC32 is computed separately via compute_header_crc32().
 * 
 * @param header Pointer to header to initialize
 * @param msg_type Message type
 * @param payload_len Length of payload in bytes
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t message_header_init(
    message_header_t* header,
    message_type_t msg_type,
    uint32_t payload_len
);

/**
 * @brief Serialize message header to buffer (network byte order)
 * 
 * Converts all multi-byte fields to big-endian and writes to buffer.
 * Buffer must be at least MESSAGE_HEADER_SIZE bytes.
 * 
 * @param header Header to serialize (host byte order)
 * @param buffer Output buffer (network byte order)
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t serialize_header(
    const message_header_t* header,
    uint8_t* buffer
);

/**
 * @brief Deserialize message header from buffer (network byte order)
 * 
 * Converts all multi-byte fields from big-endian to host byte order.
 * 
 * @param buffer Input buffer (network byte order)
 * @param header Output header (host byte order)
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t deserialize_header(
    const uint8_t* buffer,
    message_header_t* header
);

/**
 * @brief Validate message header
 * 
 * Checks:
 * - Magic number is correct
 * - Version is supported
 * - Message type is valid
 * - Payload length is reasonable
 * 
 * @param header Header to validate
 * @return true if valid, false otherwise
 */
bool validate_message_header(const message_header_t* header);

/**
 * @brief Compute and set CRC32 for header + payload
 * 
 * Computes CRC32 over the entire message (header + payload) and
 * stores it in header->crc32. The CRC field itself is zeroed before
 * computation.
 * 
 * @param header Header to update (in host byte order)
 * @param payload Payload data (or NULL if no payload)
 * @param payload_len Payload length in bytes
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t compute_header_crc32(
    message_header_t* header,
    const uint8_t* payload,
    size_t payload_len
);

/**
 * @brief Verify CRC32 of header + payload
 * 
 * Recomputes CRC32 and compares with header->crc32.
 * 
 * @param header Header to verify
 * @param payload Payload data (or NULL if no payload)
 * @param payload_len Payload length in bytes
 * @return true if CRC matches, false otherwise
 */
bool verify_message_crc32(
    const message_header_t* header,
    const uint8_t* payload,
    size_t payload_len
);

/**
 * @brief Get string representation of message type
 * 
 * @param msg_type Message type
 * @return Human-readable string (e.g., "RAFT_REQUEST_VOTE")
 */
const char* message_type_to_string(message_type_t msg_type);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_PROTOCOL_BINARY_H */