/**
 * @file tlv.h
 * @brief Type-Length-Value (TLV) Encoding/Decoding
 * 
 * Flexible binary encoding system for protocol message payloads.
 * Each field is encoded as:
 *   [Type:1 byte][Tag:2 bytes][Length:4 bytes][Value:N bytes]
 * 
 * Features:
 * - Self-describing format (type information embedded)
 * - Forward/backward compatibility (unknown fields can be skipped)
 * - Dynamic buffer growth
 * - Nested structures (arrays, maps)
 * - Zero-copy decoding where possible
 * 
 * Wire Format:
 * ┌──────────────────────────────────────┐
 * │  Byte 0    | Type (uint8_t)          │
 * │  Bytes 1-2 | Tag (uint16_t, BE)      │
 * │  Bytes 3-6 | Length (uint32_t, BE)   │
 * │  Bytes 7+  | Value (N bytes)         │
 * └──────────────────────────────────────┘
 * 
 * Example Usage:
 * @code
 * // Encoding
 * tlv_encoder_t* enc = tlv_encoder_create(256);
 * tlv_encode_uint32(enc, FIELD_TERM, 42);
 * tlv_encode_string(enc, FIELD_NODE_ID, "node-123");
 * 
 * size_t len;
 * uint8_t* buffer = tlv_encoder_finalize(enc, &len);
 * send(socket, buffer, len, 0);
 * 
 * // Decoding
 * tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
 * tlv_field_t field;
 * 
 * while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
 *     if (field.tag == FIELD_TERM) {
 *         uint32_t term = tlv_field_as_uint32(&field);
 *     }
 * }
 * 
 * tlv_decoder_free(dec);
 * @endcode
 * 
 * @version 1.0.0
 */

#ifndef DISTRIC_PROTOCOL_TLV_H
#define DISTRIC_PROTOCOL_TLV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <distric_obs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * TLV TYPE DEFINITIONS
 * ========================================================================= */

/**
 * @brief TLV field types
 * 
 * Indicates the data type of the value portion.
 */
typedef enum {
    TLV_UINT8       = 0x01,    /**< Unsigned 8-bit integer */
    TLV_UINT16      = 0x02,    /**< Unsigned 16-bit integer */
    TLV_UINT32      = 0x03,    /**< Unsigned 32-bit integer */
    TLV_UINT64      = 0x04,    /**< Unsigned 64-bit integer */
    TLV_INT8        = 0x05,    /**< Signed 8-bit integer */
    TLV_INT16       = 0x06,    /**< Signed 16-bit integer */
    TLV_INT32       = 0x07,    /**< Signed 32-bit integer */
    TLV_INT64       = 0x08,    /**< Signed 64-bit integer */
    TLV_FLOAT       = 0x09,    /**< 32-bit float */
    TLV_DOUBLE      = 0x0A,    /**< 64-bit double */
    TLV_BOOL        = 0x0B,    /**< Boolean (1 byte: 0=false, 1=true) */
    TLV_STRING      = 0x10,    /**< UTF-8 string (length-prefixed, null-terminated) */
    TLV_BYTES       = 0x11,    /**< Raw byte array (length-prefixed) */
    TLV_ARRAY       = 0x20,    /**< Array of TLV fields */
    TLV_MAP         = 0x21,    /**< Key-value pairs (both keys and values are TLV) */
} tlv_type_t;

/* ============================================================================
 * TLV FIELD STRUCTURE
 * ========================================================================= */

/**
 * @brief Decoded TLV field
 * 
 * Represents a single field extracted from a TLV buffer.
 * The value pointer points into the original buffer (zero-copy).
 */
typedef struct {
    tlv_type_t type;        /**< Field type */
    uint16_t tag;           /**< Field identifier */
    uint32_t length;        /**< Length of value in bytes */
    const uint8_t* value;   /**< Pointer to value data (in original buffer) */
} tlv_field_t;

/**
 * @section memory_ownership Memory Ownership Semantics
 * 
 * ENCODER:
 * - tlv_encoder_create() allocates internal buffer (caller must free encoder)
 * - tlv_encoder_finalize() returns pointer to INTERNAL buffer
 *   → Valid until encoder is freed or reset
 *   → Caller does NOT own this memory
 * - tlv_encoder_detach() returns buffer and transfers ownership to CALLER
 *   → Caller MUST free() the returned buffer
 *   → Encoder is reset after detach
 * 
 * DECODER:
 * - tlv_decoder_create() does NOT copy buffer
 *   → Original buffer must remain valid for decoder lifetime
 *   → Caller retains ownership of input buffer
 * - tlv_field_t.value points DIRECTLY into original buffer (zero-copy)
 *   → Valid only while original buffer exists
 *   → Do NOT free() field.value
 * - tlv_decoder_free() only frees decoder struct, NOT the buffer
 * 
 * EXAMPLE - Encoder:
 * @code
 * tlv_encoder_t* enc = tlv_encoder_create(256);
 * tlv_encode_uint32(enc, TAG, 42);
 * 
 * // Option 1: Use internal buffer (no ownership transfer)
 * size_t len;
 * uint8_t* buf = tlv_encoder_finalize(enc, &len);
 * send(socket, buf, len, 0);  // OK: buffer still owned by encoder
 * tlv_encoder_free(enc);      // Buffer freed here
 * 
 * // Option 2: Detach buffer (ownership transfer)
 * uint8_t* buf = tlv_encoder_detach(enc, &len);
 * tlv_encoder_free(enc);      // Safe: buffer already detached
 * // ...use buf later...
 * free(buf);                  // Caller must free
 * @endcode
 * 
 * EXAMPLE - Decoder:
 * @code
 * uint8_t buffer[1024];
 * recv(socket, buffer, 1024, 0);
 * 
 * tlv_decoder_t* dec = tlv_decoder_create(buffer, 1024);
 * tlv_field_t field;
 * 
 * while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
 *     const char* str = tlv_field_get_string(&field);
 *     // str points INTO buffer (zero-copy)
 *     printf("%s\n", str);  // OK: buffer still valid
 * }
 * 
 * tlv_decoder_free(dec);  // Only frees decoder, NOT buffer
 * // buffer still valid here
 * @endcode
 */

/* ============================================================================
 * TLV ENCODER
 * ========================================================================= */

/**
 * @brief TLV encoder state (opaque)
 */
typedef struct tlv_encoder tlv_encoder_t;

/**
 * @brief Create a new TLV encoder
 * 
 * Creates an encoder with an initial buffer capacity.
 * Buffer grows automatically as needed.
 * 
 * @param initial_capacity Initial buffer size in bytes
 * @return Encoder instance, or NULL on allocation failure
 */
tlv_encoder_t* tlv_encoder_create(size_t initial_capacity);

/**
 * @brief Free TLV encoder and its buffer
 * 
 * @param encoder Encoder to free
 */
void tlv_encoder_free(tlv_encoder_t* encoder);

/**
 * @brief Get current buffer size
 * 
 * @param encoder Encoder instance
 * @return Number of bytes written so far
 */
size_t tlv_encoder_size(const tlv_encoder_t* encoder);

/**
 * @brief Reset encoder (clear buffer, keep capacity)
 * 
 * @param encoder Encoder to reset
 */
void tlv_encoder_reset(tlv_encoder_t* encoder);

/* ============================================================================
 * ENCODING FUNCTIONS
 * ========================================================================= */

/**
 * @brief Encode unsigned 8-bit integer
 */
distric_err_t tlv_encode_uint8(tlv_encoder_t* enc, uint16_t tag, uint8_t value);

/**
 * @brief Encode unsigned 16-bit integer
 */
distric_err_t tlv_encode_uint16(tlv_encoder_t* enc, uint16_t tag, uint16_t value);

/**
 * @brief Encode unsigned 32-bit integer
 */
distric_err_t tlv_encode_uint32(tlv_encoder_t* enc, uint16_t tag, uint32_t value);

/**
 * @brief Encode unsigned 64-bit integer
 */
distric_err_t tlv_encode_uint64(tlv_encoder_t* enc, uint16_t tag, uint64_t value);

/**
 * @brief Encode signed 32-bit integer
 */
distric_err_t tlv_encode_int32(tlv_encoder_t* enc, uint16_t tag, int32_t value);

/**
 * @brief Encode signed 64-bit integer
 */
distric_err_t tlv_encode_int64(tlv_encoder_t* enc, uint16_t tag, int64_t value);

/**
 * @brief Encode boolean value
 */
distric_err_t tlv_encode_bool(tlv_encoder_t* enc, uint16_t tag, bool value);

/**
 * @brief Encode UTF-8 string
 * 
 * String is encoded with null terminator included.
 * 
 * @param enc Encoder instance
 * @param tag Field tag
 * @param str Null-terminated string (UTF-8)
 * @return DISTRIC_OK on success
 */
distric_err_t tlv_encode_string(tlv_encoder_t* enc, uint16_t tag, const char* str);

/**
 * @brief Encode raw byte array
 * 
 * @param enc Encoder instance
 * @param tag Field tag
 * @param data Byte array
 * @param len Length in bytes
 * @return DISTRIC_OK on success
 */
distric_err_t tlv_encode_bytes(tlv_encoder_t* enc, uint16_t tag, const uint8_t* data, size_t len);

/**
 * @brief Encode raw TLV field (pre-encoded)
 * 
 * Useful for copying fields from one buffer to another.
 * 
 * @param enc Encoder instance
 * @param type Field type
 * @param tag Field tag
 * @param data Value data
 * @param len Length of value
 * @return DISTRIC_OK on success
 */
distric_err_t tlv_encode_raw(tlv_encoder_t* enc, tlv_type_t type, uint16_t tag, 
                             const uint8_t* data, size_t len);

/* ============================================================================
 * FINALIZATION
 * ========================================================================= */

/**
 * @brief Finalize encoding and get buffer
 * 
 * Returns pointer to internal buffer. The buffer remains valid until
 * encoder is freed or reset.
 * 
 * @param encoder Encoder instance
 * @param len_out Output: buffer length in bytes
 * @return Pointer to encoded buffer, or NULL on error
 */
uint8_t* tlv_encoder_finalize(tlv_encoder_t* encoder, size_t* len_out);

/**
 * @brief Finalize and detach buffer (caller owns memory)
 * 
 * Returns buffer and transfers ownership to caller.
 * Encoder is reset after this call.
 * 
 * @param encoder Encoder instance
 * @param len_out Output: buffer length in bytes
 * @return Pointer to buffer (caller must free()), or NULL on error
 */
uint8_t* tlv_encoder_detach(tlv_encoder_t* encoder, size_t* len_out);

/* ============================================================================
 * TLV DECODER
 * ========================================================================= */

/**
 * @brief TLV decoder state (opaque)
 */
typedef struct tlv_decoder tlv_decoder_t;

/**
 * @brief Create a new TLV decoder
 * 
 * Creates a decoder that reads from the given buffer.
 * The buffer must remain valid for the lifetime of the decoder.
 * 
 * @param buffer TLV-encoded buffer
 * @param length Buffer length in bytes
 * @return Decoder instance, or NULL on allocation failure
 */
tlv_decoder_t* tlv_decoder_create(const uint8_t* buffer, size_t length);

/**
 * @brief Free TLV decoder
 * 
 * Does NOT free the buffer (decoder does not own it).
 * 
 * @param decoder Decoder to free
 */
void tlv_decoder_free(tlv_decoder_t* decoder);

/**
 * @brief Get current decoder position
 * 
 * @param decoder Decoder instance
 * @return Byte offset in buffer
 */
size_t tlv_decoder_position(const tlv_decoder_t* decoder);

/**
 * @brief Check if decoder has more data
 * 
 * @param decoder Decoder instance
 * @return true if more fields available
 */
bool tlv_decoder_has_more(const tlv_decoder_t* decoder);

/**
 * @brief Reset decoder to beginning
 * 
 * @param decoder Decoder instance
 */
void tlv_decoder_reset(tlv_decoder_t* decoder);

/* ============================================================================
 * DECODING FUNCTIONS
 * ========================================================================= */

/**
 * @brief Decode next field
 * 
 * Advances decoder position to next field.
 * The field's value pointer is a direct pointer into the buffer (zero-copy).
 * 
 * @param decoder Decoder instance
 * @param field_out Output: decoded field
 * @return DISTRIC_OK if field decoded, DISTRIC_ERR_EOF if no more fields,
 *         other error codes on corruption
 */
distric_err_t tlv_decode_next(tlv_decoder_t* decoder, tlv_field_t* field_out);

/**
 * @brief Find field by tag (linear search from current position)
 * 
 * Searches forward from current position. Does not reset position on failure.
 * 
 * @param decoder Decoder instance
 * @param tag Tag to search for
 * @param field_out Output: decoded field if found
 * @return DISTRIC_OK if found, DISTRIC_ERR_NOT_FOUND if not found
 */
distric_err_t tlv_find_field(tlv_decoder_t* decoder, uint16_t tag, tlv_field_t* field_out);

/**
 * @brief Skip current field without decoding
 * 
 * Advances decoder position past the current field.
 * 
 * @param decoder Decoder instance
 * @return DISTRIC_OK on success
 */
distric_err_t tlv_skip_field(tlv_decoder_t* decoder);

/* ============================================================================
 * FIELD VALUE EXTRACTION
 * ========================================================================= */

/**
 * @brief Extract uint8 from field
 * 
 * @param field Field to extract from
 * @param value_out Output: extracted value
 * @return DISTRIC_OK on success, DISTRIC_ERR_TYPE_MISMATCH if wrong type
 */
distric_err_t tlv_field_get_uint8(const tlv_field_t* field, uint8_t* value_out);

/**
 * @brief Extract uint16 from field
 */
distric_err_t tlv_field_get_uint16(const tlv_field_t* field, uint16_t* value_out);

/**
 * @brief Extract uint32 from field
 */
distric_err_t tlv_field_get_uint32(const tlv_field_t* field, uint32_t* value_out);

/**
 * @brief Extract uint64 from field
 */
distric_err_t tlv_field_get_uint64(const tlv_field_t* field, uint64_t* value_out);

/**
 * @brief Extract int32 from field
 */
distric_err_t tlv_field_get_int32(const tlv_field_t* field, int32_t* value_out);

/**
 * @brief Extract int64 from field
 */
distric_err_t tlv_field_get_int64(const tlv_field_t* field, int64_t* value_out);

/**
 * @brief Extract bool from field
 */
distric_err_t tlv_field_get_bool(const tlv_field_t* field, bool* value_out);

/**
 * @brief Extract string from field
 * 
 * Returns pointer to null-terminated string in original buffer (zero-copy).
 * 
 * @param field Field to extract from
 * @return Pointer to string, or NULL if wrong type
 */
const char* tlv_field_get_string(const tlv_field_t* field);

/**
 * @brief Extract byte array from field
 * 
 * Returns pointer to data in original buffer (zero-copy).
 * 
 * @param field Field to extract from
 * @param len_out Output: length of byte array
 * @return Pointer to data, or NULL if wrong type
 */
const uint8_t* tlv_field_get_bytes(const tlv_field_t* field, size_t* len_out);

/* ============================================================================
 * CONVENIENCE EXTRACTORS (with default values)
 * ========================================================================= */

/**
 * @brief Extract uint32 with default value if type mismatch
 * 
 * Useful for optional fields or forward compatibility.
 */
uint32_t tlv_field_as_uint32(const tlv_field_t* field);

/**
 * @brief Extract uint64 with default value if type mismatch
 */
uint64_t tlv_field_as_uint64(const tlv_field_t* field);

/**
 * @brief Extract string with default value if type mismatch
 * 
 * @return String pointer or empty string if type mismatch
 */
const char* tlv_field_as_string(const tlv_field_t* field);

/* ============================================================================
 * VALIDATION
 * ========================================================================= */

/**
 * @brief Validate TLV buffer structure
 * 
 * Performs a full scan of the buffer to check for:
 * - Valid field headers
 * - Lengths within buffer bounds
 * - No truncated fields
 * 
 * @param buffer TLV-encoded buffer
 * @param length Buffer length
 * @return true if valid, false if corrupted
 */
bool tlv_validate_buffer(const uint8_t* buffer, size_t length);

/**
 * @brief Get type name as string
 * 
 * @param type TLV type
 * @return Type name (e.g., "UINT32", "STRING")
 */
const char* tlv_type_to_string(tlv_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_PROTOCOL_TLV_H */