/**
 * @file tlv.c
 * @brief TLV Encoder/Decoder Implementation
 * 
 * Provides dynamic buffer management for encoding and zero-copy
 * decoding with forward compatibility (unknown fields can be skipped).
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "distric_protocol/tlv.h"
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>  /* htonl, htons, ntohl, ntohs */

/* ============================================================================
 * BYTE ORDER CONVERSION (64-bit)
 * ========================================================================= */

static inline uint64_t htonll(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)(value & 0xFFFFFFFF)) << 32) | 
           htonl((uint32_t)(value >> 32));
#else
    return value;
#endif
}

static inline uint64_t ntohll(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)ntohl((uint32_t)(value & 0xFFFFFFFF)) << 32) | 
           ntohl((uint32_t)(value >> 32));
#else
    return value;
#endif
}

/* ============================================================================
 * TLV FIELD HEADER
 * ========================================================================= */

/** Size of TLV field header: [type:1][tag:2][length:4] = 7 bytes */
#define TLV_HEADER_SIZE 7

/**
 * @brief Write TLV field header to buffer
 * 
 * @param buffer Output buffer (must have at least 7 bytes)
 * @param type Field type
 * @param tag Field tag
 * @param length Value length
 */
static inline void write_tlv_header(uint8_t* buffer, tlv_type_t type, 
                                    uint16_t tag, uint32_t length) {
    buffer[0] = (uint8_t)type;
    *(uint16_t*)(buffer + 1) = htons(tag);
    *(uint32_t*)(buffer + 3) = htonl(length);
}

/**
 * @brief Read TLV field header from buffer
 * 
 * @param buffer Input buffer
 * @param type_out Output: field type
 * @param tag_out Output: field tag
 * @param length_out Output: value length
 */
static inline void read_tlv_header(const uint8_t* buffer, tlv_type_t* type_out,
                                   uint16_t* tag_out, uint32_t* length_out) {
    *type_out = (tlv_type_t)buffer[0];
    *tag_out = ntohs(*(const uint16_t*)(buffer + 1));
    *length_out = ntohl(*(const uint32_t*)(buffer + 3));
}

/* ============================================================================
 * ENCODER IMPLEMENTATION
 * ========================================================================= */

struct tlv_encoder {
    uint8_t* buffer;
    size_t capacity;
    size_t offset;
};

tlv_encoder_t* tlv_encoder_create(size_t initial_capacity) {
    if (initial_capacity == 0) {
        initial_capacity = 256;
    }
    
    tlv_encoder_t* enc = (tlv_encoder_t*)malloc(sizeof(tlv_encoder_t));
    if (!enc) {
        return NULL;
    }
    
    enc->buffer = (uint8_t*)malloc(initial_capacity);
    if (!enc->buffer) {
        free(enc);
        return NULL;
    }
    
    enc->capacity = initial_capacity;
    enc->offset = 0;
    
    return enc;
}

void tlv_encoder_free(tlv_encoder_t* encoder) {
    if (!encoder) {
        return;
    }
    
    free(encoder->buffer);
    free(encoder);
}

size_t tlv_encoder_size(const tlv_encoder_t* encoder) {
    return encoder ? encoder->offset : 0;
}

void tlv_encoder_reset(tlv_encoder_t* encoder) {
    if (encoder) {
        encoder->offset = 0;
    }
}

/**
 * @brief Ensure encoder has enough capacity
 * 
 * Grows buffer if needed (doubles capacity).
 */
static distric_err_t ensure_capacity(tlv_encoder_t* enc, size_t required) {
    if (!enc) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (enc->offset + required <= enc->capacity) {
        return DISTRIC_OK;  /* Sufficient capacity */
    }
    
    /* Calculate new capacity (double until sufficient) */
    size_t new_capacity = enc->capacity;
    while (new_capacity < enc->offset + required) {
        new_capacity *= 2;
    }
    
    /* Reallocate buffer */
    uint8_t* new_buffer = (uint8_t*)realloc(enc->buffer, new_capacity);
    if (!new_buffer) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    enc->buffer = new_buffer;
    enc->capacity = new_capacity;
    
    return DISTRIC_OK;
}

/* ============================================================================
 * RAW ENCODING (used by all type-specific encoders)
 * ========================================================================= */

distric_err_t tlv_encode_raw(tlv_encoder_t* enc, tlv_type_t type, uint16_t tag,
                             const uint8_t* data, size_t len) {
    if (!enc) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Ensure capacity for header + data */
    distric_err_t err = ensure_capacity(enc, TLV_HEADER_SIZE + len);
    if (err != DISTRIC_OK) {
        return err;
    }
    
    /* Write header */
    write_tlv_header(enc->buffer + enc->offset, type, tag, (uint32_t)len);
    enc->offset += TLV_HEADER_SIZE;
    
    /* Write data */
    if (data && len > 0) {
        memcpy(enc->buffer + enc->offset, data, len);
        enc->offset += len;
    }
    
    return DISTRIC_OK;
}

/* ============================================================================
 * TYPE-SPECIFIC ENCODING
 * ========================================================================= */

distric_err_t tlv_encode_uint8(tlv_encoder_t* enc, uint16_t tag, uint8_t value) {
    return tlv_encode_raw(enc, TLV_UINT8, tag, &value, sizeof(uint8_t));
}

distric_err_t tlv_encode_uint16(tlv_encoder_t* enc, uint16_t tag, uint16_t value) {
    uint16_t value_be = htons(value);
    return tlv_encode_raw(enc, TLV_UINT16, tag, (uint8_t*)&value_be, sizeof(uint16_t));
}

distric_err_t tlv_encode_uint32(tlv_encoder_t* enc, uint16_t tag, uint32_t value) {
    uint32_t value_be = htonl(value);
    return tlv_encode_raw(enc, TLV_UINT32, tag, (uint8_t*)&value_be, sizeof(uint32_t));
}

distric_err_t tlv_encode_uint64(tlv_encoder_t* enc, uint16_t tag, uint64_t value) {
    uint64_t value_be = htonll(value);
    return tlv_encode_raw(enc, TLV_UINT64, tag, (uint8_t*)&value_be, sizeof(uint64_t));
}

distric_err_t tlv_encode_int32(tlv_encoder_t* enc, uint16_t tag, int32_t value) {
    uint32_t value_be = htonl((uint32_t)value);
    return tlv_encode_raw(enc, TLV_INT32, tag, (uint8_t*)&value_be, sizeof(int32_t));
}

distric_err_t tlv_encode_int64(tlv_encoder_t* enc, uint16_t tag, int64_t value) {
    uint64_t value_be = htonll((uint64_t)value);
    return tlv_encode_raw(enc, TLV_INT64, tag, (uint8_t*)&value_be, sizeof(int64_t));
}

distric_err_t tlv_encode_bool(tlv_encoder_t* enc, uint16_t tag, bool value) {
    uint8_t val = value ? 1 : 0;
    return tlv_encode_raw(enc, TLV_BOOL, tag, &val, sizeof(uint8_t));
}

distric_err_t tlv_encode_string(tlv_encoder_t* enc, uint16_t tag, const char* str) {
    if (!str) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Include null terminator */
    size_t len = strlen(str) + 1;
    return tlv_encode_raw(enc, TLV_STRING, tag, (const uint8_t*)str, len);
}

distric_err_t tlv_encode_bytes(tlv_encoder_t* enc, uint16_t tag, 
                               const uint8_t* data, size_t len) {
    return tlv_encode_raw(enc, TLV_BYTES, tag, data, len);
}

/* ============================================================================
 * FINALIZATION
 * ========================================================================= */

uint8_t* tlv_encoder_finalize(tlv_encoder_t* encoder, size_t* len_out) {
    if (!encoder || !len_out) {
        return NULL;
    }
    
    *len_out = encoder->offset;
    return encoder->buffer;
}

uint8_t* tlv_encoder_detach(tlv_encoder_t* encoder, size_t* len_out) {
    if (!encoder || !len_out) {
        return NULL;
    }
    
    uint8_t* buffer = encoder->buffer;
    *len_out = encoder->offset;
    
    /* Reset encoder (buffer ownership transferred to caller) */
    encoder->buffer = NULL;
    encoder->capacity = 0;
    encoder->offset = 0;
    
    return buffer;
}

/* ============================================================================
 * DECODER IMPLEMENTATION
 * ========================================================================= */

struct tlv_decoder {
    const uint8_t* buffer;
    size_t length;
    size_t offset;
};

tlv_decoder_t* tlv_decoder_create(const uint8_t* buffer, size_t length) {
    if (!buffer || length == 0) {
        return NULL;
    }
    
    tlv_decoder_t* dec = (tlv_decoder_t*)malloc(sizeof(tlv_decoder_t));
    if (!dec) {
        return NULL;
    }
    
    dec->buffer = buffer;
    dec->length = length;
    dec->offset = 0;
    
    return dec;
}

void tlv_decoder_free(tlv_decoder_t* decoder) {
    free(decoder);
}

size_t tlv_decoder_position(const tlv_decoder_t* decoder) {
    return decoder ? decoder->offset : 0;
}

bool tlv_decoder_has_more(const tlv_decoder_t* decoder) {
    if (!decoder) {
        return false;
    }
    
    return decoder->offset < decoder->length;
}

void tlv_decoder_reset(tlv_decoder_t* decoder) {
    if (decoder) {
        decoder->offset = 0;
    }
}

/* ============================================================================
 * DECODING
 * ========================================================================= */

distric_err_t tlv_decode_next(tlv_decoder_t* decoder, tlv_field_t* field_out) {
    if (!decoder || !field_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Check if we have enough bytes for header */
    if (decoder->offset + TLV_HEADER_SIZE > decoder->length) {
        return DISTRIC_ERR_EOF;
    }
    
    /* Read header */
    const uint8_t* header = decoder->buffer + decoder->offset;
    tlv_type_t type;
    uint16_t tag;
    uint32_t length;
    
    read_tlv_header(header, &type, &tag, &length);
    
    /* Advance past header */
    decoder->offset += TLV_HEADER_SIZE;
    
    /* Check if we have enough bytes for value */
    if (decoder->offset + length > decoder->length) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    /* Fill output field (zero-copy: points into original buffer) */
    field_out->type = type;
    field_out->tag = tag;
    field_out->length = length;
    field_out->value = decoder->buffer + decoder->offset;
    
    /* Advance past value */
    decoder->offset += length;
    
    return DISTRIC_OK;
}

distric_err_t tlv_find_field(tlv_decoder_t* decoder, uint16_t tag, 
                             tlv_field_t* field_out) {
    if (!decoder || !field_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Linear search from current position */
    while (tlv_decoder_has_more(decoder)) {
        size_t saved_offset = decoder->offset;
        
        distric_err_t err = tlv_decode_next(decoder, field_out);
        if (err != DISTRIC_OK) {
            return err;
        }
        
        if (field_out->tag == tag) {
            /* Found! Rewind to start of this field */
            decoder->offset = saved_offset;
            
            /* Decode again to set field_out correctly */
            return tlv_decode_next(decoder, field_out);
        }
    }
    
    return DISTRIC_ERR_NOT_FOUND;
}

distric_err_t tlv_skip_field(tlv_decoder_t* decoder) {
    if (!decoder) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    /* Check if we have enough bytes for header */
    if (decoder->offset + TLV_HEADER_SIZE > decoder->length) {
        return DISTRIC_ERR_EOF;
    }
    
    /* Read header to get length */
    const uint8_t* header = decoder->buffer + decoder->offset;
    tlv_type_t type;
    uint16_t tag;
    uint32_t length;
    
    read_tlv_header(header, &type, &tag, &length);
    
    /* Skip header + value */
    decoder->offset += TLV_HEADER_SIZE + length;
    
    /* Check bounds */
    if (decoder->offset > decoder->length) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    return DISTRIC_OK;
}

/* ============================================================================
 * FIELD VALUE EXTRACTION (with type checking)
 * ========================================================================= */

distric_err_t tlv_field_get_uint8(const tlv_field_t* field, uint8_t* value_out) {
    if (!field || !value_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (field->type != TLV_UINT8) {
        return DISTRIC_ERR_TYPE_MISMATCH;
    }
    
    if (field->length != sizeof(uint8_t)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    *value_out = field->value[0];
    return DISTRIC_OK;
}

distric_err_t tlv_field_get_uint16(const tlv_field_t* field, uint16_t* value_out) {
    if (!field || !value_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (field->type != TLV_UINT16) {
        return DISTRIC_ERR_TYPE_MISMATCH;
    }
    
    if (field->length != sizeof(uint16_t)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    *value_out = ntohs(*(const uint16_t*)field->value);
    return DISTRIC_OK;
}

distric_err_t tlv_field_get_uint32(const tlv_field_t* field, uint32_t* value_out) {
    if (!field || !value_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (field->type != TLV_UINT32) {
        return DISTRIC_ERR_TYPE_MISMATCH;
    }
    
    if (field->length != sizeof(uint32_t)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    *value_out = ntohl(*(const uint32_t*)field->value);
    return DISTRIC_OK;
}

distric_err_t tlv_field_get_uint64(const tlv_field_t* field, uint64_t* value_out) {
    if (!field || !value_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (field->type != TLV_UINT64) {
        return DISTRIC_ERR_TYPE_MISMATCH;
    }
    
    if (field->length != sizeof(uint64_t)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    *value_out = ntohll(*(const uint64_t*)field->value);
    return DISTRIC_OK;
}

distric_err_t tlv_field_get_int32(const tlv_field_t* field, int32_t* value_out) {
    if (!field || !value_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (field->type != TLV_INT32) {
        return DISTRIC_ERR_TYPE_MISMATCH;
    }
    
    if (field->length != sizeof(int32_t)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    uint32_t temp = ntohl(*(const uint32_t*)field->value);
    *value_out = (int32_t)temp;
    return DISTRIC_OK;
}

distric_err_t tlv_field_get_int64(const tlv_field_t* field, int64_t* value_out) {
    if (!field || !value_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (field->type != TLV_INT64) {
        return DISTRIC_ERR_TYPE_MISMATCH;
    }
    
    if (field->length != sizeof(int64_t)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    uint64_t temp = ntohll(*(const uint64_t*)field->value);
    *value_out = (int64_t)temp;
    return DISTRIC_OK;
}

distric_err_t tlv_field_get_bool(const tlv_field_t* field, bool* value_out) {
    if (!field || !value_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (field->type != TLV_BOOL) {
        return DISTRIC_ERR_TYPE_MISMATCH;
    }
    
    if (field->length != sizeof(uint8_t)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    *value_out = field->value[0] != 0;
    return DISTRIC_OK;
}

const char* tlv_field_get_string(const tlv_field_t* field) {
    if (!field || field->type != TLV_STRING || field->length == 0) {
        return NULL;
    }
    
    /* Verify null terminator is present */
    if (field->value[field->length - 1] != '\0') {
        return NULL;
    }
    
    return (const char*)field->value;
}

const uint8_t* tlv_field_get_bytes(const tlv_field_t* field, size_t* len_out) {
    if (!field || !len_out) {
        return NULL;
    }
    
    if (field->type != TLV_BYTES) {
        return NULL;
    }
    
    *len_out = field->length;
    return field->value;
}

/* ============================================================================
 * CONVENIENCE EXTRACTORS (with default values)
 * ========================================================================= */

uint32_t tlv_field_as_uint32(const tlv_field_t* field) {
    uint32_t value = 0;
    tlv_field_get_uint32(field, &value);
    return value;
}

uint64_t tlv_field_as_uint64(const tlv_field_t* field) {
    uint64_t value = 0;
    tlv_field_get_uint64(field, &value);
    return value;
}

const char* tlv_field_as_string(const tlv_field_t* field) {
    const char* str = tlv_field_get_string(field);
    return str ? str : "";
}

/* ============================================================================
 * VALIDATION
 * ========================================================================= */

bool tlv_validate_buffer(const uint8_t* buffer, size_t length) {
    if (!buffer || length == 0) {
        return false;
    }
    
    size_t offset = 0;
    
    while (offset < length) {
        /* Check header */
        if (offset + TLV_HEADER_SIZE > length) {
            return false;  /* Truncated header */
        }
        
        /* Read header */
        tlv_type_t type;
        uint16_t tag;
        uint32_t field_length;
        
        read_tlv_header(buffer + offset, &type, &tag, &field_length);
        offset += TLV_HEADER_SIZE;
        
        /* Check value length */
        if (offset + field_length > length) {
            return false;  /* Truncated value */
        }
        
        offset += field_length;
    }
    
    return offset == length;  /* Must consume entire buffer */
}

const char* tlv_type_to_string(tlv_type_t type) {
    switch (type) {
        case TLV_UINT8:     return "UINT8";
        case TLV_UINT16:    return "UINT16";
        case TLV_UINT32:    return "UINT32";
        case TLV_UINT64:    return "UINT64";
        case TLV_INT8:      return "INT8";
        case TLV_INT16:     return "INT16";
        case TLV_INT32:     return "INT32";
        case TLV_INT64:     return "INT64";
        case TLV_FLOAT:     return "FLOAT";
        case TLV_DOUBLE:    return "DOUBLE";
        case TLV_BOOL:      return "BOOL";
        case TLV_STRING:    return "STRING";
        case TLV_BYTES:     return "BYTES";
        case TLV_ARRAY:     return "ARRAY";
        case TLV_MAP:       return "MAP";
        default:            return "UNKNOWN";
    }
}