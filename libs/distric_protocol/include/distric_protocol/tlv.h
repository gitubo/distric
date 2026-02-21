#ifndef DISTRIC_PROTOCOL_TLV_H
#define DISTRIC_PROTOCOL_TLV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <distric_obs.h>           /* distric_err_t */
#include "distric_protocol/limits.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * TLV TYPE CODES
 * ========================================================================= */

typedef enum {
    TLV_UINT8   = 0x01,
    TLV_UINT16  = 0x02,
    TLV_UINT32  = 0x03,
    TLV_UINT64  = 0x04,
    TLV_INT8    = 0x05,
    TLV_INT16   = 0x06,
    TLV_INT32   = 0x07,
    TLV_INT64   = 0x08,
    TLV_FLOAT   = 0x09,
    TLV_DOUBLE  = 0x0A,
    TLV_BOOL    = 0x0B,
    TLV_STRING  = 0x10,
    TLV_BYTES   = 0x11,
    TLV_ARRAY   = 0x20,
    TLV_MAP     = 0x21,
} tlv_type_t;

/* ============================================================================
 * BORROWED FIELD  (zero-copy — value points into original buffer)
 * ========================================================================= */

typedef struct {
    tlv_type_t     type;
    uint16_t       tag;
    uint32_t       length;
    const uint8_t *value;   /* points into decoder's source buffer — do NOT free */
} tlv_field_t;

/* ============================================================================
 * OWNED FIELD  (deep-copied — caller must free)
 * ========================================================================= */

typedef struct {
    tlv_type_t  type;
    uint16_t    tag;
    uint32_t    length;
    uint8_t    *value;    /* heap-allocated copy; call tlv_field_owned_free() */
} tlv_field_owned_t;

/* Free memory associated with an owned field. */
void tlv_field_owned_free(tlv_field_owned_t *field);

/* ============================================================================
 * ENCODER
 * ========================================================================= */

typedef struct tlv_encoder tlv_encoder_t;

tlv_encoder_t *tlv_encoder_create(size_t initial_capacity);
void           tlv_encoder_free(tlv_encoder_t *encoder);
size_t         tlv_encoder_size(const tlv_encoder_t *encoder);
void           tlv_encoder_reset(tlv_encoder_t *encoder);

distric_err_t tlv_encode_raw(tlv_encoder_t *enc, tlv_type_t type, uint16_t tag,
                              const uint8_t *data, size_t len);
distric_err_t tlv_encode_uint8(tlv_encoder_t *enc,  uint16_t tag, uint8_t  value);
distric_err_t tlv_encode_uint16(tlv_encoder_t *enc, uint16_t tag, uint16_t value);
distric_err_t tlv_encode_uint32(tlv_encoder_t *enc, uint16_t tag, uint32_t value);
distric_err_t tlv_encode_uint64(tlv_encoder_t *enc, uint16_t tag, uint64_t value);
distric_err_t tlv_encode_int32(tlv_encoder_t *enc,  uint16_t tag, int32_t  value);
distric_err_t tlv_encode_int64(tlv_encoder_t *enc,  uint16_t tag, int64_t  value);
distric_err_t tlv_encode_bool(tlv_encoder_t *enc,   uint16_t tag, bool     value);
distric_err_t tlv_encode_string(tlv_encoder_t *enc, uint16_t tag, const char *str);
distric_err_t tlv_encode_bytes(tlv_encoder_t *enc,  uint16_t tag,
                                const uint8_t *data, size_t len);

/* Returns pointer to internal buffer (encoder owns it). */
uint8_t *tlv_encoder_finalize(tlv_encoder_t *encoder, size_t *len_out);
/* Detaches buffer — caller must free(). Encoder is reset. */
uint8_t *tlv_encoder_detach(tlv_encoder_t *encoder, size_t *len_out);

/* ============================================================================
 * DECODER
 * ========================================================================= */

typedef struct tlv_decoder tlv_decoder_t;

/* Basic decoder — no limits enforcement. */
tlv_decoder_t *tlv_decoder_create(const uint8_t *buffer, size_t length);

/*
 * Limits-aware decoder.
 * Enforces max_fields and max_string_len during iteration.
 * limits may be NULL (uses production defaults).
 */
tlv_decoder_t *tlv_decoder_create_with_limits(const uint8_t          *buffer,
                                               size_t                  length,
                                               const protocol_limits_t *limits);

void   tlv_decoder_free(tlv_decoder_t *decoder);
size_t tlv_decoder_position(const tlv_decoder_t *decoder);
bool   tlv_decoder_has_more(const tlv_decoder_t *decoder);
void   tlv_decoder_reset(tlv_decoder_t *decoder);

/* Decode next field (zero-copy). */
distric_err_t tlv_decode_next(tlv_decoder_t *decoder, tlv_field_t *field_out);

/*
 * Decode next field with deep-copy of value (owned mode).
 * Returns DISTRIC_ERR_NO_MEMORY on allocation failure.
 * Caller MUST call tlv_field_owned_free() on success.
 */
distric_err_t tlv_decode_next_owned(tlv_decoder_t *decoder, tlv_field_owned_t *out);

/* Linear scan for first field with matching tag. */
distric_err_t tlv_find_field(tlv_decoder_t *decoder, uint16_t tag,
                              tlv_field_t *field_out);

distric_err_t tlv_skip_field(tlv_decoder_t *decoder);

/* ============================================================================
 * INDEXED DECODER  (O(1) lookup after one-time index build)
 * ========================================================================= */

/*
 * Build a tag→buffer-offset index by scanning the entire buffer once.
 * After this call, tlv_decoder_find_indexed() resolves in O(log n).
 *
 * The index is stored inside the decoder; free() via tlv_decoder_free().
 * Resets the decoder position to 0.
 *
 * Returns DISTRIC_ERR_NO_MEMORY if the index cannot be allocated.
 */
distric_err_t tlv_decoder_build_index(tlv_decoder_t *decoder);

/*
 * Find a field by tag using the pre-built index.
 * Requires a prior successful call to tlv_decoder_build_index().
 * Returns DISTRIC_ERR_INVALID_STATE if no index exists.
 * Returns DISTRIC_ERR_NOT_FOUND    if the tag is absent.
 */
distric_err_t tlv_decoder_find_indexed(const tlv_decoder_t *decoder,
                                        uint16_t             tag,
                                        tlv_field_t         *field_out);

/* ============================================================================
 * VALUE EXTRACTORS
 * ========================================================================= */

uint8_t        tlv_field_get_uint8(const tlv_field_t *field);
uint16_t       tlv_field_get_uint16(const tlv_field_t *field);
uint32_t       tlv_field_get_uint32(const tlv_field_t *field);
uint64_t       tlv_field_get_uint64(const tlv_field_t *field);
int32_t        tlv_field_get_int32(const tlv_field_t *field);
int64_t        tlv_field_get_int64(const tlv_field_t *field);
bool           tlv_field_get_bool(const tlv_field_t *field);
const char    *tlv_field_get_string(const tlv_field_t *field);
const uint8_t *tlv_field_get_bytes(const tlv_field_t *field, size_t *len_out);

/* Convenience extractors with safe defaults on type mismatch. */
uint32_t    tlv_field_as_uint32(const tlv_field_t *field);
uint64_t    tlv_field_as_uint64(const tlv_field_t *field);
const char *tlv_field_as_string(const tlv_field_t *field);

/* ============================================================================
 * VALIDATION
 * ========================================================================= */

bool        tlv_validate_buffer(const uint8_t *buffer, size_t length);
const char *tlv_type_to_string(tlv_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_PROTOCOL_TLV_H */