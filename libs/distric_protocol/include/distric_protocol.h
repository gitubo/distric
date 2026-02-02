/**
 * @file distric_protocol.h
 * @brief DistriC Protocol Layer - Single Public Header
 * 
 * This is the ONLY header file that users of the distric_protocol library
 * need to include. It provides:
 * - Binary protocol message header structures
 * - Message serialization/deserialization
 * - CRC32 checksum computation
 * - TLV (Type-Length-Value) encoding/decoding
 * - Message type definitions
 * 
 * The protocol uses a fixed 32-byte header followed by variable-length payload.
 * Payloads are encoded using TLV format for flexibility and forward compatibility.
 * All multi-byte values are in network byte order (big-endian).
 * 
 * Usage Example:
 * @code
 * #include <distric_protocol.h>
 * 
 * // ===== Encoding a Message =====
 * 
 * // 1. Create TLV payload
 * tlv_encoder_t* enc = tlv_encoder_create(256);
 * tlv_encode_uint32(enc, FIELD_TERM, 42);
 * tlv_encode_string(enc, FIELD_CANDIDATE_ID, "node-123");
 * 
 * size_t payload_len;
 * uint8_t* payload = tlv_encoder_finalize(enc, &payload_len);
 * 
 * // 2. Create message header
 * message_header_t header;
 * message_header_init(&header, MSG_RAFT_REQUEST_VOTE, payload_len);
 * 
 * // 3. Compute CRC32 over header + payload
 * compute_header_crc32(&header, payload, payload_len);
 * 
 * // 4. Serialize header
 * uint8_t header_buf[MESSAGE_HEADER_SIZE];
 * serialize_header(&header, header_buf);
 * 
 * // 5. Send: header_buf (32 bytes) + payload
 * send(socket, header_buf, MESSAGE_HEADER_SIZE, 0);
 * send(socket, payload, payload_len, 0);
 * 
 * tlv_encoder_free(enc);
 * 
 * // ===== Decoding a Message =====
 * 
 * // 1. Receive header
 * uint8_t header_buf[MESSAGE_HEADER_SIZE];
 * recv(socket, header_buf, MESSAGE_HEADER_SIZE, 0);
 * 
 * // 2. Deserialize header
 * message_header_t header;
 * deserialize_header(header_buf, &header);
 * 
 * // 3. Validate header
 * if (!validate_message_header(&header)) {
 *     // Invalid header
 *     return;
 * }
 * 
 * // 4. Receive payload
 * uint8_t* payload = malloc(header.payload_len);
 * recv(socket, payload, header.payload_len, 0);
 * 
 * // 5. Verify CRC32
 * if (!verify_message_crc32(&header, payload, header.payload_len)) {
 *     // Corruption detected
 *     free(payload);
 *     return;
 * }
 * 
 * // 6. Decode TLV payload
 * tlv_decoder_t* dec = tlv_decoder_create(payload, header.payload_len);
 * tlv_field_t field;
 * 
 * while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
 *     switch (field.tag) {
 *         case FIELD_TERM:
 *             uint32_t term = tlv_field_as_uint32(&field);
 *             break;
 *         case FIELD_CANDIDATE_ID:
 *             const char* id = tlv_field_as_string(&field);
 *             break;
 *     }
 * }
 * 
 * tlv_decoder_free(dec);
 * free(payload);
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

/**
 * @defgroup tlv TLV Encoding
 * @brief Type-Length-Value flexible payload encoding
 * 
 * Provides self-describing binary format with:
 * - Dynamic buffer growth
 * - Zero-copy decoding
 * - Forward compatibility (unknown fields can be skipped)
 * - Type safety
 * - Network byte order
 * @{
 */
#include "distric_protocol/tlv.h"
/** @} */

/**
 * @defgroup messages Protocol Messages
 * @brief All protocol message definitions and serialization
 * 
 * Provides complete message structures for:
 * - Raft consensus (RequestVote, AppendEntries, InstallSnapshot)
 * - Gossip protocol (Ping, Ack, MembershipUpdate)
 * - Task execution (Assignment, Result, Status)
 * - Client API (Submit, Response, Query, Error)
 * 
 * All messages use TLV encoding internally.
 * @{
 */
#include "distric_protocol/messages.h"
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

/* ============================================================================
 * TYPICAL USAGE WORKFLOW
 * ========================================================================= */

/**
 * @page protocol_workflow Protocol Usage Workflow
 * 
 * @section encoding Encoding Messages
 * 
 * 1. Create TLV encoder: `tlv_encoder_create()`
 * 2. Encode fields: `tlv_encode_uint32()`, `tlv_encode_string()`, etc.
 * 3. Finalize payload: `tlv_encoder_finalize()`
 * 4. Initialize header: `message_header_init()`
 * 5. Compute CRC32: `compute_header_crc32()`
 * 6. Serialize header: `serialize_header()`
 * 7. Send header + payload over network
 * 8. Cleanup: `tlv_encoder_free()`
 * 
 * @section decoding Decoding Messages
 * 
 * 1. Receive header (32 bytes)
 * 2. Deserialize header: `deserialize_header()`
 * 3. Validate header: `validate_message_header()`
 * 4. Receive payload (header.payload_len bytes)
 * 5. Verify CRC32: `verify_message_crc32()`
 * 6. Create TLV decoder: `tlv_decoder_create()`
 * 7. Decode fields: `tlv_decode_next()` in loop
 * 8. Extract values: `tlv_field_get_uint32()`, etc.
 * 9. Cleanup: `tlv_decoder_free()`
 */

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_PROTOCOL_H */