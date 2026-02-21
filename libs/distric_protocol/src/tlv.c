#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "distric_protocol/tlv.h"
#include "distric_protocol/error_info.h"
#include "distric_protocol/byteswap.h"

#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

/* ============================================================================
 * FIELD-SIZE LIMIT (retain existing per-field cap, now also via limits_t)
 * ========================================================================= */

#ifndef TLV_MAX_FIELD_SIZE
#  define TLV_MAX_FIELD_SIZE (16u * 1024u * 1024u)
#endif

#if TLV_MAX_FIELD_SIZE == 0 || TLV_MAX_FIELD_SIZE > (256u * 1024u * 1024u)
#  error "TLV_MAX_FIELD_SIZE must be in [1, 268435456]"
#endif

/* ============================================================================
 * TLV WIRE HEADER: [type:1][tag:2][length:4] = 7 bytes, big-endian
 * ========================================================================= */

#define TLV_HEADER_SIZE 7u

static inline void write_tlv_header(uint8_t *buf, tlv_type_t type,
                                    uint16_t tag, uint32_t length)
{
    uint16_t tag_be    = htons(tag);
    uint32_t length_be = htonl(length);
    buf[0] = (uint8_t)type;
    memcpy(buf + 1, &tag_be,    2);
    memcpy(buf + 3, &length_be, 4);
}

static inline void read_tlv_header(const uint8_t *buf, tlv_type_t *type_out,
                                   uint16_t *tag_out, uint32_t *length_out)
{
    uint16_t tag_be; uint32_t length_be;
    *type_out = (tlv_type_t)buf[0];
    memcpy(&tag_be,    buf + 1, 2);
    memcpy(&length_be, buf + 3, 4);
    *tag_out    = ntohs(tag_be);
    *length_out = ntohl(length_be);
}

/* ============================================================================
 * INDEX ENTRY
 * ========================================================================= */

typedef struct { uint16_t tag; size_t buf_offset; } tlv_idx_entry_t;

/* ============================================================================
 * ENCODER
 * ========================================================================= */

struct tlv_encoder {
    uint8_t *buffer;
    size_t   capacity;
    size_t   offset;
};

tlv_encoder_t *tlv_encoder_create(size_t initial_capacity)
{
    if (initial_capacity == 0) initial_capacity = 256;
    tlv_encoder_t *enc = (tlv_encoder_t*)malloc(sizeof(tlv_encoder_t));
    if (!enc) return NULL;
    enc->buffer = (uint8_t*)malloc(initial_capacity);
    if (!enc->buffer) { free(enc); return NULL; }
    enc->capacity = initial_capacity;
    enc->offset   = 0;
    return enc;
}

void tlv_encoder_free(tlv_encoder_t *encoder)
{
    if (!encoder) return;
    free(encoder->buffer);
    free(encoder);
}

size_t tlv_encoder_size(const tlv_encoder_t *encoder)
{
    return encoder ? encoder->offset : 0;
}

void tlv_encoder_reset(tlv_encoder_t *encoder)
{
    if (encoder) encoder->offset = 0;
}

static distric_err_t ensure_capacity(tlv_encoder_t *enc, size_t required)
{
    if (enc->offset + required <= enc->capacity) return DISTRIC_OK;
    size_t new_cap = enc->capacity ? enc->capacity : 256;
    while (new_cap < enc->offset + required) new_cap *= 2;
    uint8_t *nb = (uint8_t*)realloc(enc->buffer, new_cap);
    if (!nb) return DISTRIC_ERR_NO_MEMORY;
    enc->buffer   = nb;
    enc->capacity = new_cap;
    return DISTRIC_OK;
}

distric_err_t tlv_encode_raw(tlv_encoder_t *enc, tlv_type_t type, uint16_t tag,
                              const uint8_t *data, size_t len)
{
    if (!enc) return DISTRIC_ERR_INVALID_ARG;
    if (len > 0 && !data) return DISTRIC_ERR_INVALID_ARG;
    if (len > TLV_MAX_FIELD_SIZE) return DISTRIC_ERR_INVALID_ARG;

    size_t total = TLV_HEADER_SIZE + len;
    distric_err_t err = ensure_capacity(enc, total);
    if (err != DISTRIC_OK) return err;

    write_tlv_header(enc->buffer + enc->offset, type, tag, (uint32_t)len);
    enc->offset += TLV_HEADER_SIZE;
    if (len > 0) { memcpy(enc->buffer + enc->offset, data, len); enc->offset += len; }
    return DISTRIC_OK;
}

distric_err_t tlv_encode_uint8(tlv_encoder_t *enc, uint16_t tag, uint8_t value)
    { return tlv_encode_raw(enc, TLV_UINT8, tag, &value, 1); }

distric_err_t tlv_encode_uint16(tlv_encoder_t *enc, uint16_t tag, uint16_t value)
    { uint16_t be=htons(value); return tlv_encode_raw(enc,TLV_UINT16,tag,(const uint8_t*)&be,2); }

distric_err_t tlv_encode_uint32(tlv_encoder_t *enc, uint16_t tag, uint32_t value)
    { uint32_t be=htonl(value); return tlv_encode_raw(enc,TLV_UINT32,tag,(const uint8_t*)&be,4); }

distric_err_t tlv_encode_uint64(tlv_encoder_t *enc, uint16_t tag, uint64_t value)
    { uint64_t be=htonll(value); return tlv_encode_raw(enc,TLV_UINT64,tag,(const uint8_t*)&be,8); }

distric_err_t tlv_encode_int32(tlv_encoder_t *enc, uint16_t tag, int32_t value)
    { uint32_t be=htonl((uint32_t)value); return tlv_encode_raw(enc,TLV_INT32,tag,(const uint8_t*)&be,4); }

distric_err_t tlv_encode_int64(tlv_encoder_t *enc, uint16_t tag, int64_t value)
    { uint64_t be=htonll((uint64_t)value); return tlv_encode_raw(enc,TLV_INT64,tag,(const uint8_t*)&be,8); }

distric_err_t tlv_encode_bool(tlv_encoder_t *enc, uint16_t tag, bool value)
    { uint8_t v=value?1u:0u; return tlv_encode_raw(enc,TLV_BOOL,tag,&v,1); }

distric_err_t tlv_encode_string(tlv_encoder_t *enc, uint16_t tag, const char *str)
{
    if (!str) return DISTRIC_ERR_INVALID_ARG;
    size_t len = strlen(str);
    if (len >= TLV_MAX_FIELD_SIZE) return DISTRIC_ERR_INVALID_ARG;
    return tlv_encode_raw(enc, TLV_STRING, tag, (const uint8_t*)str, len+1u);
}

distric_err_t tlv_encode_bytes(tlv_encoder_t *enc, uint16_t tag,
                                const uint8_t *data, size_t len)
    { return tlv_encode_raw(enc, TLV_BYTES, tag, data, len); }

uint8_t *tlv_encoder_finalize(tlv_encoder_t *encoder, size_t *len_out)
{
    if (!encoder || !len_out) return NULL;
    *len_out = encoder->offset;
    return encoder->buffer;
}

uint8_t *tlv_encoder_detach(tlv_encoder_t *encoder, size_t *len_out)
{
    if (!encoder || !len_out) return NULL;
    uint8_t *buf = encoder->buffer;
    *len_out = encoder->offset;
    encoder->buffer   = NULL;
    encoder->capacity = 0;
    encoder->offset   = 0;
    return buf;
}

/* ============================================================================
 * DECODER
 * ========================================================================= */

struct tlv_decoder {
    const uint8_t    *buffer;
    size_t            length;
    size_t            offset;
    /* limits */
    uint32_t          max_fields;
    uint32_t          max_string_len;
    uint32_t          fields_decoded;
    /* index (built on demand) */
    tlv_idx_entry_t  *index;
    uint32_t          index_size;    /* number of entries */
    uint32_t          index_cap;
};

tlv_decoder_t *tlv_decoder_create(const uint8_t *buffer, size_t length)
{
    return tlv_decoder_create_with_limits(buffer, length, NULL);
}

tlv_decoder_t *tlv_decoder_create_with_limits(const uint8_t          *buffer,
                                               size_t                  length,
                                               const protocol_limits_t *limits)
{
    if (!buffer || length == 0) return NULL;
    tlv_decoder_t *dec = (tlv_decoder_t*)calloc(1, sizeof(tlv_decoder_t));
    if (!dec) return NULL;
    dec->buffer         = buffer;
    dec->length         = length;
    dec->offset         = 0;
    dec->max_fields     = protocol_limits_max_fields(limits);
    dec->max_string_len = protocol_limits_max_string(limits);
    dec->fields_decoded = 0;
    dec->index          = NULL;
    dec->index_size     = 0;
    dec->index_cap      = 0;
    return dec;
}

void tlv_decoder_free(tlv_decoder_t *decoder)
{
    if (!decoder) return;
    free(decoder->index);
    free(decoder);
}

size_t tlv_decoder_position(const tlv_decoder_t *decoder)
{
    return decoder ? decoder->offset : 0;
}

bool tlv_decoder_has_more(const tlv_decoder_t *decoder)
{
    if (!decoder) return false;
    return decoder->offset + TLV_HEADER_SIZE <= decoder->length;
}

void tlv_decoder_reset(tlv_decoder_t *decoder)
{
    if (!decoder) return;
    decoder->offset         = 0;
    decoder->fields_decoded = 0;
}

/* Core decode step (shared by borrowed and owned paths) */
static distric_err_t decode_next_internal(tlv_decoder_t *decoder,
                                           tlv_type_t    *type_out,
                                           uint16_t      *tag_out,
                                           uint32_t      *length_out,
                                           size_t        *value_offset_out)
{
    if (decoder->offset >= decoder->length) return DISTRIC_ERR_EOF;
    if (decoder->offset + TLV_HEADER_SIZE > decoder->length) {
        distric_set_error_info(DISTRIC_ERR_INVALID_FORMAT, PROTO_STAGE_TLV_DECODE,
                               (uint32_t)decoder->offset, 0, "truncated TLV header");
        return DISTRIC_ERR_INVALID_FORMAT;
    }

    /* Limits: field count */
    if (decoder->max_fields > 0 && decoder->fields_decoded >= decoder->max_fields) {
        distric_set_error_info(DISTRIC_ERR_INVALID_FORMAT, PROTO_STAGE_LIMITS_CHECK,
                               (uint32_t)decoder->offset, 0, "max_fields exceeded");
        return DISTRIC_ERR_INVALID_FORMAT;
    }

    tlv_type_t type; uint16_t tag; uint32_t length;
    read_tlv_header(decoder->buffer + decoder->offset, &type, &tag, &length);

    /* Hard cap per field */
    if (length > TLV_MAX_FIELD_SIZE) {
        distric_set_error_info(DISTRIC_ERR_INVALID_FORMAT, PROTO_STAGE_TLV_DECODE,
                               (uint32_t)decoder->offset, tag,
                               "field length exceeds TLV_MAX_FIELD_SIZE");
        return DISTRIC_ERR_INVALID_FORMAT;
    }

    /* Limits: string/bytes cap */
    if (decoder->max_string_len > 0 &&
        (type == TLV_STRING || type == TLV_BYTES) &&
        length > decoder->max_string_len) {
        distric_set_error_info(DISTRIC_ERR_INVALID_FORMAT, PROTO_STAGE_LIMITS_CHECK,
                               (uint32_t)decoder->offset, tag,
                               "string/bytes field exceeds max_string_len");
        return DISTRIC_ERR_INVALID_FORMAT;
    }

    size_t remaining = decoder->length - decoder->offset - TLV_HEADER_SIZE;
    if (length > remaining) {
        distric_set_error_info(DISTRIC_ERR_INVALID_FORMAT, PROTO_STAGE_TLV_DECODE,
                               (uint32_t)decoder->offset, tag,
                               "field length exceeds buffer");
        return DISTRIC_ERR_INVALID_FORMAT;
    }

    *type_out         = type;
    *tag_out          = tag;
    *length_out       = length;
    *value_offset_out = decoder->offset + TLV_HEADER_SIZE;

    decoder->offset += TLV_HEADER_SIZE + length;
    decoder->fields_decoded++;
    return DISTRIC_OK;
}

distric_err_t tlv_decode_next(tlv_decoder_t *decoder, tlv_field_t *field_out)
{
    if (!decoder || !field_out) return DISTRIC_ERR_INVALID_ARG;

    tlv_type_t type; uint16_t tag; uint32_t length; size_t voff;
    distric_err_t err = decode_next_internal(decoder, &type, &tag, &length, &voff);
    if (err != DISTRIC_OK) return err;

    field_out->type   = type;
    field_out->tag    = tag;
    field_out->length = length;
    field_out->value  = decoder->buffer + voff;
    return DISTRIC_OK;
}

distric_err_t tlv_decode_next_owned(tlv_decoder_t *decoder, tlv_field_owned_t *out)
{
    if (!decoder || !out) return DISTRIC_ERR_INVALID_ARG;

    tlv_type_t type; uint16_t tag; uint32_t length; size_t voff;
    distric_err_t err = decode_next_internal(decoder, &type, &tag, &length, &voff);
    if (err != DISTRIC_OK) return err;

    out->type   = type;
    out->tag    = tag;
    out->length = length;
    out->value  = NULL;

    if (length > 0) {
        out->value = (uint8_t*)malloc(length);
        if (!out->value) return DISTRIC_ERR_NO_MEMORY;
        memcpy(out->value, decoder->buffer + voff, length);
    }
    return DISTRIC_OK;
}

void tlv_field_owned_free(tlv_field_owned_t *field)
{
    if (!field) return;
    free(field->value);
    field->value = NULL;
}

distric_err_t tlv_find_field(tlv_decoder_t *decoder, uint16_t tag,
                              tlv_field_t *field_out)
{
    if (!decoder || !field_out) return DISTRIC_ERR_INVALID_ARG;

    tlv_field_t   field;
    distric_err_t err;
    while ((err = tlv_decode_next(decoder, &field)) == DISTRIC_OK) {
        if (field.tag == tag) { *field_out = field; return DISTRIC_OK; }
    }
    return DISTRIC_ERR_NOT_FOUND;
}

distric_err_t tlv_skip_field(tlv_decoder_t *decoder)
{
    if (!decoder) return DISTRIC_ERR_INVALID_ARG;
    tlv_field_t f;
    return tlv_decode_next(decoder, &f);
}

/* ============================================================================
 * INDEXED DECODER
 * ========================================================================= */

/* Comparison for bsearch / qsort */
static int idx_cmp(const void *a, const void *b)
{
    const tlv_idx_entry_t *ea = (const tlv_idx_entry_t*)a;
    const tlv_idx_entry_t *eb = (const tlv_idx_entry_t*)b;
    return (ea->tag > eb->tag) - (ea->tag < eb->tag);
}

distric_err_t tlv_decoder_build_index(tlv_decoder_t *decoder)
{
    if (!decoder) return DISTRIC_ERR_INVALID_ARG;

    free(decoder->index);
    decoder->index      = NULL;
    decoder->index_size = 0;
    decoder->index_cap  = 0;

    tlv_decoder_reset(decoder);

    uint32_t cap = 16;
    tlv_idx_entry_t *idx = (tlv_idx_entry_t*)malloc(cap * sizeof(tlv_idx_entry_t));
    if (!idx) return DISTRIC_ERR_NO_MEMORY;

    while (decoder->offset + TLV_HEADER_SIZE <= decoder->length) {
        /* Guard field count */
        if (decoder->max_fields > 0 && decoder->index_size >= decoder->max_fields)
            break;

        size_t entry_offset = decoder->offset;
        tlv_type_t type; uint16_t tag; uint32_t length;
        read_tlv_header(decoder->buffer + decoder->offset, &type, &tag, &length);

        /* Safety checks */
        if (length > TLV_MAX_FIELD_SIZE) break;
        size_t remaining = decoder->length - decoder->offset - TLV_HEADER_SIZE;
        if (length > remaining) break;

        if (decoder->index_size >= cap) {
            uint32_t new_cap = cap * 2;
            tlv_idx_entry_t *nb = (tlv_idx_entry_t*)realloc(idx,
                                    new_cap * sizeof(tlv_idx_entry_t));
            if (!nb) { free(idx); return DISTRIC_ERR_NO_MEMORY; }
            idx = nb; cap = new_cap;
        }

        idx[decoder->index_size].tag        = tag;
        idx[decoder->index_size].buf_offset = entry_offset;
        decoder->index_size++;

        decoder->offset += TLV_HEADER_SIZE + length;
    }

    qsort(idx, decoder->index_size, sizeof(tlv_idx_entry_t), idx_cmp);

    decoder->index     = idx;
    decoder->index_cap = cap;

    tlv_decoder_reset(decoder);
    return DISTRIC_OK;
}

distric_err_t tlv_decoder_find_indexed(const tlv_decoder_t *decoder,
                                        uint16_t             tag,
                                        tlv_field_t         *field_out)
{
    if (!decoder || !field_out) return DISTRIC_ERR_INVALID_ARG;
    if (!decoder->index)        return DISTRIC_ERR_INVALID_STATE;

    tlv_idx_entry_t key = { .tag = tag, .buf_offset = 0 };
    tlv_idx_entry_t *found = (tlv_idx_entry_t*)bsearch(
        &key, decoder->index, decoder->index_size,
        sizeof(tlv_idx_entry_t), idx_cmp);

    if (!found) return DISTRIC_ERR_NOT_FOUND;

    tlv_type_t type; uint16_t ftag; uint32_t length;
    read_tlv_header(decoder->buffer + found->buf_offset, &type, &ftag, &length);

    field_out->type   = type;
    field_out->tag    = ftag;
    field_out->length = length;
    field_out->value  = decoder->buffer + found->buf_offset + TLV_HEADER_SIZE;
    return DISTRIC_OK;
}

/* ============================================================================
 * VALUE EXTRACTORS
 * ========================================================================= */

uint8_t tlv_field_get_uint8(const tlv_field_t *f)
{
    if (!f || f->type != TLV_UINT8 || f->length < 1) return 0;
    return f->value[0];
}

uint16_t tlv_field_get_uint16(const tlv_field_t *f)
{
    if (!f || f->type != TLV_UINT16 || f->length < 2) return 0;
    uint16_t v; memcpy(&v, f->value, 2); return ntohs(v);
}

uint32_t tlv_field_get_uint32(const tlv_field_t *f)
{
    if (!f || f->type != TLV_UINT32 || f->length < 4) return 0;
    uint32_t v; memcpy(&v, f->value, 4); return ntohl(v);
}

uint64_t tlv_field_get_uint64(const tlv_field_t *f)
{
    if (!f || f->type != TLV_UINT64 || f->length < 8) return 0;
    uint64_t v; memcpy(&v, f->value, 8); return ntohll(v);
}

int32_t tlv_field_get_int32(const tlv_field_t *f)
{
    if (!f || f->type != TLV_INT32 || f->length < 4) return 0;
    uint32_t v; memcpy(&v, f->value, 4); return (int32_t)ntohl(v);
}

int64_t tlv_field_get_int64(const tlv_field_t *f)
{
    if (!f || f->type != TLV_INT64 || f->length < 8) return 0;
    uint64_t v; memcpy(&v, f->value, 8); return (int64_t)ntohll(v);
}

bool tlv_field_get_bool(const tlv_field_t *f)
{
    if (!f || f->type != TLV_BOOL || f->length < 1) return false;
    return f->value[0] != 0;
}

const char *tlv_field_get_string(const tlv_field_t *f)
{
    if (!f || f->type != TLV_STRING || f->length == 0) return NULL;
    return (const char*)f->value;
}

const uint8_t *tlv_field_get_bytes(const tlv_field_t *f, size_t *len_out)
{
    if (!f || f->type != TLV_BYTES) { if (len_out) *len_out=0; return NULL; }
    if (len_out) *len_out = f->length;
    return f->value;
}

uint32_t tlv_field_as_uint32(const tlv_field_t *f)
{
    if (!f) return 0;
    if (f->type == TLV_UINT32 && f->length >= 4) return tlv_field_get_uint32(f);
    if (f->type == TLV_UINT16 && f->length >= 2) return tlv_field_get_uint16(f);
    if (f->type == TLV_UINT8  && f->length >= 1) return tlv_field_get_uint8(f);
    return 0;
}

uint64_t tlv_field_as_uint64(const tlv_field_t *f)
{
    if (!f) return 0;
    if (f->type == TLV_UINT64 && f->length >= 8) return tlv_field_get_uint64(f);
    return (uint64_t)tlv_field_as_uint32(f);
}

const char *tlv_field_as_string(const tlv_field_t *f)
{
    const char *s = tlv_field_get_string(f);
    return s ? s : "";
}

/* ============================================================================
 * VALIDATION
 * ========================================================================= */

bool tlv_validate_buffer(const uint8_t *buffer, size_t length)
{
    if (!buffer || length == 0) return true;
    size_t offset = 0;
    while (offset < length) {
        if (offset + TLV_HEADER_SIZE > length) return false;
        tlv_type_t type; uint16_t tag; uint32_t flen;
        read_tlv_header(buffer + offset, &type, &tag, &flen);
        if (flen > TLV_MAX_FIELD_SIZE) return false;
        size_t remaining = length - offset - TLV_HEADER_SIZE;
        if (flen > remaining) return false;
        offset += TLV_HEADER_SIZE + flen;
    }
    return true;
}

const char *tlv_type_to_string(tlv_type_t type)
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