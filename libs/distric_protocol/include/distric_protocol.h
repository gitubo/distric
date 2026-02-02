/**
 * @file distric_protocol.h
 * @brief DistriC Protocol Layer - Single Public Header
 * 
 * This is the ONLY header file that users of the distric_protocol library
 * need to include. It provides:
 * - Binary protocol message header structures
 * - Message serialization/deserialization
 * - CRC32 checksum computation
 * - Message type definitions
 * 
 * The protocol uses a fixed 32-byte header followed by variable-length payload.
 * All multi-byte values are in network byte order (big-endian).
 * 
 * Usage:
 * @code
 * #include <distric_protocol.h>
 * 
 * // Create a message header
 * message_header_t header;
 * message_header_init(&header, MSG_RAFT_REQUEST_VOTE, payload_len);
 * 
 * // Serialize to buffer
 * uint8_t buffer[MESSAGE_HEADER_SIZE];
 * serialize_header(&header, buffer);
 * 
 * // Compute CRC32 over header + payload
 * uint32_t crc = compute_crc32(buffer, MESSAGE_HEADER_SIZE);
 * 
 * // Deserialize from buffer
 * message_header_t received;
 * deserialize_header(buffer, &received);
 * 
 * // Validate
 * if (validate_message_header(&received, payload, payload_len)) {
 *     // Process message
 * }
 * @endcode
 * 
 * @version 1.0.0
 * @author DistriC Development Team
 */

#ifndef DISTRIC_PROTOCOL_H
#define DISTRIC_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * PROTOCOL LAYER MODULES
 * ========================================================================= */

/**
 * @defgroup binary Binary Protocol
 * @brief Fixed 32-byte message header with CRC32 checksums
 * 
 * Provides portable binary serialization with:
 * - Network byte order (big-endian)
 * - Magic number validation
 * - Version negotiation
 * - Message type identification
 * - CRC32 corruption detection
 * @{
 */
#include "distric_protocol/binary.h"
#include "distric_protocol/crc32.h"
/** @} */

/* ============================================================================
 * VERSION INFORMATION
 * ========================================================================= */

#define DISTRIC_PROTOCOL_VERSION_MAJOR 1
#define DISTRIC_PROTOCOL_VERSION_MINOR 0
#define DISTRIC_PROTOCOL_VERSION_PATCH 0

/**
 * @brief Get library version string
 * 
 * @return Version string (e.g., "1.0.0")
 */
static inline const char* distric_protocol_version(void) {
    return "1.0.0";
}

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_PROTOCOL_H */