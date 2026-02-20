/**
 * @file tlv.c
 * @brief TLV Encoder/Decoder Implementation
 *
 * Provides dynamic buffer management for encoding and zero-copy decoding
 * with forward compatibility (unknown fields are transparently skipped).
 *
 * Fix #1 — Strict-aliasing / unaligned-access UB:
 *   write_tlv_header() and read_tlv_header() previously cast uint8_t* to
 *   uint16_t* / uint32_t* for multi-byte field-header reads and writes.
 *   All such accesses now use memcpy() with local integer variables, which is
 *   the only portable and standards-compliant approach.
 *
 *   The same UB existed in the field-value extractors (tlv_field_get_uint16,
 *   _uint32, _uint64, _int32, _int64).  All are fixed here with the same
 *   memcpy + ntohX pattern.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "distric_protocol/tlv.h"
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>  /* htonl, htons, ntohl, ntohs */

/* ============================================================================
 * PORTABLE 64-BIT BYTE-SWAP (duplicated from binary.c to keep the TLV module
 * self-contained and avoid creating a shared internal header).
 * ========================================================================= */

#ifndef htonll
# if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
static inline uint64_t htonll(uint64_t v)
{
    return ((uint64_t)htonl((uint32_t)(v & 0xFFFFFFFFu)) << 32)
         | ((uint64_t)htonl((uint32_t)(v >> 32)));
}
static inline uint64_t ntohll(uint64_t v) { return htonll(v); }
# else
static inline uint64_t htonll(uint64_t v) { return v; }
static inline uint64_t ntohll(uint64_t v) { return v; }
# endif
#endif

/* ============================================================================
 * TLV FIELD HEADER
 *
 * Wire format: [type:1][tag:2][length:4] = 7 bytes, all fields big-endian.
 * ========================================================================= */

#define TLV_HEADER_SIZE 7

/**
 * @brief Write a TLV field header into a buffer (Fix #1: memcpy).
 *
 * @param buffer  Output — must have at least TLV_HEADER_SIZE bytes available.
 */
static inline void write_tlv_header(uint8_t*   buffer,
                                    tlv_type_t type,
                                    uint16_t   tag,
                                    uint32_t   length)
{
    uint16_t tag_be    = htons(tag);
    uint32_t length_be = htonl(length);

    buffer[0] = (uint8_t)type;
    memcpy(buffer + 1, &tag_be,    2);
    memcpy(buffer + 3, &length_be, 4);
}

/**
 * @brief Read a TLV field header from a buffer (Fix #1: memcpy).
 *
 * @param buffer  Input — must have at least TLV_HEADER_SIZE bytes available.
 */
static inline void read_tlv_header(const uint8_t* buffer,
                                   tlv_type_t*    type_out,
                                   uint16_t*      tag_out,
                                   uint32_t*      length_out)
{
    uint16_t tag_be;
    uint32_t length_be;

    *type_out = (tlv_type_t)buffer[0];
    memcpy(&tag_be,    buffer + 1, 2);
    memcpy(&length_be, buffer + 3, 4);

    *tag_out    = ntohs(tag_be);
    *length_out = ntohl(length_be);
}

/* ============================================================================
 * ENCODER IMPLEMENTATION
 * ========================================================================= */

struct tlv_encoder {
    uint8_t* buffer;
    size_t   capacity;
    size_t   offset;
};

tlv_encoder_t* tlv_encoder_create(size_t initial_capacity)
{
    if (initial_capacity == 0) {
        initial_capacity = 256;
    }

    tlv_encoder_t* enc = (tlv_encoder_t*)malloc(sizeof(tlv_encoder_t));
    if (!enc) return NULL;

    enc->buffer = (uint8_t*)malloc(initial_capacity);
    if (!enc->buffer) {
        free(enc);
        return NULL;
    }

    enc->capacity = initial_capacity;
    enc->offset   = 0;
    return enc;
}

void tlv_encoder_free(tlv_encoder_t* encoder)
{
    if (!encoder) return;
    free(encoder->buffer);
    free(encoder);
}

size_t tlv_encoder_size(const tlv_encoder_t* encoder)
{
    return encoder ? encoder->offset : 0;
}

void tlv_encoder_reset(tlv_encoder_t* encoder)
{
    if (encoder) encoder->offset = 0;
}

/* Ensure the encoder has at least `required` bytes of free space. */
static distric_err_t ensure_capacity(tlv_encoder_t* enc, size_t required)
{
    if (!enc) return DISTRIC_ERR_INVALID_ARG;

    if (enc->offset + required <= enc->capacity) return DISTRIC_OK;

    size_t new_cap = enc->capacity ? enc->capacity : 256;
    while (new_cap < enc->offset + required) {
        new_cap *= 2;
    }

    uint8_t* nb = (uint8_t*)realloc(enc->buffer, new_cap);
    if (!nb) return DISTRIC_ERR_NO_MEMORY;

    enc->buffer   = nb;
    enc->capacity = new_cap;
    return DISTRIC_OK;
}

/* ============================================================================
 * RAW ENCODING
 * ========================================================================= */

distric_err_t tlv_encode_raw(tlv_encoder_t* enc, tlv_type_t type, uint16_t tag,
                              const uint8_t* data, size_t len)
{
    if (!enc) return DISTRIC_ERR_INVALID_ARG;
    if (len > 0 && !data) return DISTRIC_ERR_INVALID_ARG;
    if (len > 0xFFFFFFFFu) return DISTRIC_ERR_INVALID_ARG;

    size_t total = TLV_HEADER_SIZE + len;
    distric_err_t err = ensure_capacity(enc, total);
    if (err != DISTRIC_OK) return err;

    write_tlv_header(enc->buffer + enc->offset, type, tag, (uint32_t)len);
    enc->offset += TLV_HEADER_SIZE;

    if (len > 0) {
        memcpy(enc->buffer + enc->offset, data, len);
        enc->offset += len;
    }

    return DISTRIC_OK;
}

/* ============================================================================
 * TYPE-SPECIFIC ENCODERS
 * ========================================================================= */

distric_err_t tlv_encode_uint8(tlv_encoder_t* enc, uint16_t tag, uint8_t value)
{
    return tlv_encode_raw(enc, TLV_UINT8, tag, &value, sizeof(uint8_t));
}

distric_err_t tlv_encode_uint16(tlv_encoder_t* enc, uint16_t tag, uint16_t value)
{
    uint16_t be = htons(value);
    return tlv_encode_raw(enc, TLV_UINT16, tag, (const uint8_t*)&be, sizeof(uint16_t));
}

distric_err_t tlv_encode_uint32(tlv_encoder_t* enc, uint16_t tag, uint32_t value)
{
    uint32_t be = htonl(value);
    return tlv_encode_raw(enc, TLV_UINT32, tag, (const uint8_t*)&be, sizeof(uint32_t));
}

distric_err_t tlv_encode_uint64(tlv_encoder_t* enc, uint16_t tag, uint64_t value)
{
    uint64_t be = htonll(value);
    return tlv_encode_raw(enc, TLV_UINT64, tag, (const uint8_t*)&be, sizeof(uint64_t));
}

distric_err_t tlv_encode_int32(tlv_encoder_t* enc, uint16_t tag, int32_t value)
{
    uint32_t be = htonl((uint32_t)value);
    return tlv_encode_raw(enc, TLV_INT32, tag, (const uint8_t*)&be, sizeof(uint32_t));
}

distric_err_t tlv_encode_int64(tlv_encoder_t* enc, uint16_t tag, int64_t value)
{
    uint64_t be = htonll((uint64_t)value);
    return tlv_encode_raw(enc, TLV_INT64, tag, (const uint8_t*)&be, sizeof(uint64_t));
}

distric_err_t tlv_encode_bool(tlv_encoder_t* enc, uint16_t tag, bool value)
{
    uint8_t v = value ? 1u : 0u;
    return tlv_encode_raw(enc, TLV_BOOL, tag, &v, sizeof(uint8_t));
}

distric_err_t tlv_encode_string(tlv_encoder_t* enc, uint16_t tag, const char* str)
{
    if (!str) return DISTRIC_ERR_INVALID_ARG;
    size_t len = strlen(str) + 1; /* include null terminator */
    return tlv_encode_raw(enc, TLV_STRING, tag, (const uint8_t*)str, len);
}

distric_err_t tlv_encode_bytes(tlv_encoder_t* enc, uint16_t tag,
                                const uint8_t* data, size_t len)
{
    return tlv_encode_raw(enc, TLV_BYTES, tag, data, len);
}

/* ============================================================================
 * FINALISATION
 * ========================================================================= */

uint8_t* tlv_encoder_finalize(tlv_encoder_t* encoder, size_t* len_out)
{
    if (!encoder || !len_out) return NULL;
    *len_out = encoder->offset;
    return encoder->buffer;
}

uint8_t* tlv_encoder_detach(tlv_encoder_t* encoder, size_t* len_out)
{
    if (!encoder || !len_out) return NULL;

    uint8_t* buf  = encoder->buffer;
    *len_out      = encoder->offset;

    encoder->buffer   = NULL;
    encoder->capacity = 0;
    encoder->offset   = 0;

    return buf;
}

/* ============================================================================
 * DECODER IMPLEMENTATION
 * ========================================================================= */

struct tlv_decoder {
    const uint8_t* buffer;
    size_t         length;
    size_t         offset;
};

tlv_decoder_t* tlv_decoder_create(const uint8_t* buffer, size_t length)
{
    if (!buffer || length == 0) return NULL;

    tlv_decoder_t* dec = (tlv_decoder_t*)malloc(sizeof(tlv_decoder_t));
    if (!dec) return NULL;

    dec->buffer = buffer;
    dec->length = length;
    dec->offset = 0;
    return dec;
}

void tlv_decoder_free(tlv_decoder_t* decoder)
{
    free(decoder);
}

size_t tlv_decoder_position(const tlv_decoder_t* decoder)
{
    return decoder ? decoder->offset : 0;
}

bool tlv_decoder_has_more(const tlv_decoder_t* decoder)
{
    return decoder && (decoder->offset < decoder->length);
}

void tlv_decoder_reset(tlv_decoder_t* decoder)
{
    if (decoder) decoder->offset = 0;
}

/* ============================================================================
 * DECODING
 * ========================================================================= */

distric_err_t tlv_decode_next(tlv_decoder_t* decoder, tlv_field_t* field_out)
{
    if (!decoder || !field_out) return DISTRIC_ERR_INVALID_ARG;

    if (decoder->offset + TLV_HEADER_SIZE > decoder->length) {
        return DISTRIC_ERR_EOF;
    }

    tlv_type_t type;
    uint16_t   tag;
    uint32_t   length;

    read_tlv_header(decoder->buffer + decoder->offset, &type, &tag, &length);
    decoder->offset += TLV_HEADER_SIZE;

    if (decoder->offset + length > decoder->length) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }

    field_out->type   = type;
    field_out->tag    = tag;
    field_out->length = length;
    field_out->value  = decoder->buffer + decoder->offset;  /* zero-copy */
    decoder->offset  += length;

    return DISTRIC_OK;
}

distric_err_t tlv_find_field(tlv_decoder_t* decoder, uint16_t tag,
                              tlv_field_t* field_out)
{
    if (!decoder || !field_out) return DISTRIC_ERR_INVALID_ARG;

    while (tlv_decoder_has_more(decoder)) {
        size_t saved = decoder->offset;
        distric_err_t err = tlv_decode_next(decoder, field_out);
        if (err != DISTRIC_OK) return err;
        if (field_out->tag == tag) {
            decoder->offset = saved;
            return tlv_decode_next(decoder, field_out);
        }
    }

    return DISTRIC_ERR_NOT_FOUND;
}

distric_err_t tlv_skip_field(tlv_decoder_t* decoder)
{
    if (!decoder) return DISTRIC_ERR_INVALID_ARG;

    if (decoder->offset + TLV_HEADER_SIZE > decoder->length) {
        return DISTRIC_ERR_EOF;
    }

    tlv_type_t type;
    uint16_t   tag;
    uint32_t   length;

    read_tlv_header(decoder->buffer + decoder->offset, &type, &tag, &length);
    decoder->offset += TLV_HEADER_SIZE + length;

    if (decoder->offset > decoder->length) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }

    return DISTRIC_OK;
}

/* ============================================================================
 * FIELD VALUE EXTRACTION — Fix #1
 *
 * All multi-byte reads use memcpy() + ntohX() to avoid strict-aliasing UB
 * and to guarantee correctness on architectures that require alignment.
 * ========================================================================= */

distric_err_t tlv_field_get_uint8(const tlv_field_t* field, uint8_t* value_out)
{
    if (!field || !value_out)                  return DISTRIC_ERR_INVALID_ARG;
    if (field->type != TLV_UINT8)              return DISTRIC_ERR_TYPE_MISMATCH;
    if (field->length != sizeof(uint8_t))      return DISTRIC_ERR_INVALID_FORMAT;

    *value_out = field->value[0];
    return DISTRIC_OK;
}

distric_err_t tlv_field_get_uint16(const tlv_field_t* field, uint16_t* value_out)
{
    if (!field || !value_out)                  return DISTRIC_ERR_INVALID_ARG;
    if (field->type != TLV_UINT16)             return DISTRIC_ERR_TYPE_MISMATCH;
    if (field->length != sizeof(uint16_t))     return DISTRIC_ERR_INVALID_FORMAT;

    uint16_t tmp;
    memcpy(&tmp, field->value, 2);
    *value_out = ntohs(tmp);
    return DISTRIC_OK;
}

distric_err_t tlv_field_get_uint32(const tlv_field_t* field, uint32_t* value_out)
{
    if (!field || !value_out)                  return DISTRIC_ERR_INVALID_ARG;
    if (field->type != TLV_UINT32)             return DISTRIC_ERR_TYPE_MISMATCH;
    if (field->length != sizeof(uint32_t))     return DISTRIC_ERR_INVALID_FORMAT;

    uint32_t tmp;
    memcpy(&tmp, field->value, 4);
    *value_out = ntohl(tmp);
    return DISTRIC_OK;
}

distric_err_t tlv_field_get_uint64(const tlv_field_t* field, uint64_t* value_out)
{
    if (!field || !value_out)                  return DISTRIC_ERR_INVALID_ARG;
    if (field->type != TLV_UINT64)             return DISTRIC_ERR_TYPE_MISMATCH;
    if (field->length != sizeof(uint64_t))     return DISTRIC_ERR_INVALID_FORMAT;

    uint64_t tmp;
    memcpy(&tmp, field->value, 8);
    *value_out = ntohll(tmp);
    return DISTRIC_OK;
}

distric_err_t tlv_field_get_int32(const tlv_field_t* field, int32_t* value_out)
{
    if (!field || !value_out)                  return DISTRIC_ERR_INVALID_ARG;
    if (field->type != TLV_INT32)              return DISTRIC_ERR_TYPE_MISMATCH;
    if (field->length != sizeof(int32_t))      return DISTRIC_ERR_INVALID_FORMAT;

    uint32_t tmp;
    memcpy(&tmp, field->value, 4);
    *value_out = (int32_t)ntohl(tmp);
    return DISTRIC_OK;
}

distric_err_t tlv_field_get_int64(const tlv_field_t* field, int64_t* value_out)
{
    if (!field || !value_out)                  return DISTRIC_ERR_INVALID_ARG;
    if (field->type != TLV_INT64)              return DISTRIC_ERR_TYPE_MISMATCH;
    if (field->length != sizeof(int64_t))      return DISTRIC_ERR_INVALID_FORMAT;

    uint64_t tmp;
    memcpy(&tmp, field->value, 8);
    *value_out = (int64_t)ntohll(tmp);
    return DISTRIC_OK;
}

distric_err_t tlv_field_get_bool(const tlv_field_t* field, bool* value_out)
{
    if (!field || !value_out)                  return DISTRIC_ERR_INVALID_ARG;
    if (field->type != TLV_BOOL)               return DISTRIC_ERR_TYPE_MISMATCH;
    if (field->length != sizeof(uint8_t))      return DISTRIC_ERR_INVALID_FORMAT;

    *value_out = (field->value[0] != 0);
    return DISTRIC_OK;
}

const char* tlv_field_get_string(const tlv_field_t* field)
{
    if (!field || field->type != TLV_STRING || field->length == 0) return NULL;
    /* Verify null terminator present at expected position */
    if (field->value[field->length - 1] != '\0') return NULL;
    return (const char*)field->value;
}

const uint8_t* tlv_field_get_bytes(const tlv_field_t* field, size_t* len_out)
{
    if (!field || !len_out)         return NULL;
    if (field->type != TLV_BYTES)   return NULL;
    *len_out = field->length;
    return field->value;
}

/* ============================================================================
 * CONVENIENCE EXTRACTORS
 * ========================================================================= */

uint32_t tlv_field_as_uint32(const tlv_field_t* field)
{
    uint32_t v = 0;
    tlv_field_get_uint32(field, &v);
    return v;
}

uint64_t tlv_field_as_uint64(const tlv_field_t* field)
{
    uint64_t v = 0;
    tlv_field_get_uint64(field, &v);
    return v;
}

const char* tlv_field_as_string(const tlv_field_t* field)
{
    const char* s = tlv_field_get_string(field);
    return s ? s : "";
}

/* ============================================================================
 * VALIDATION
 * ========================================================================= */

bool tlv_validate_buffer(const uint8_t* buffer, size_t length)
{
    if (!buffer || length == 0) return false;

    size_t offset = 0;
    while (offset < length) {
        if (offset + TLV_HEADER_SIZE > length) return false;

        tlv_type_t type;
        uint16_t   tag;
        uint32_t   field_len;
        read_tlv_header(buffer + offset, &type, &tag, &field_len);
        offset += TLV_HEADER_SIZE;

        if (offset + field_len > length) return false;
        offset += field_len;
    }

    return (offset == length);
}

const char* tlv_type_to_string(tlv_type_t type)
{
    switch (type) {
        case TLV_UINT8:  return "UINT8";
        case TLV_UINT16: return "UINT16";
        case TLV_UINT32: return "UINT32";
        case TLV_UINT64: return "UINT64";
        case TLV_INT8:   return "INT8";
        case TLV_INT16:  return "INT16";
        case TLV_INT32:  return "INT32";
        case TLV_INT64:  return "INT64";
        case TLV_FLOAT:  return "FLOAT";
        case TLV_DOUBLE: return "DOUBLE";
        case TLV_BOOL:   return "BOOL";
        case TLV_STRING: return "STRING";
        case TLV_BYTES:  return "BYTES";
        case TLV_ARRAY:  return "ARRAY";
        case TLV_MAP:    return "MAP";
        default:         return "UNKNOWN";
    }
}