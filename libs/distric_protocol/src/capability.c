#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "distric_protocol/capability.h"
#include "distric_protocol/tlv.h"
#include "distric_protocol/binary.h"
#include "distric_protocol/error_info.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

distric_err_t capability_serialize(
    const capability_msg_t *cap,
    uint8_t               **buf_out,
    size_t                 *len_out)
{
    if (!cap || !buf_out || !len_out) return DISTRIC_ERR_INVALID_ARG;

    tlv_encoder_t *enc = tlv_encoder_create(128);
    if (!enc) return DISTRIC_ERR_NO_MEMORY;

    distric_err_t err = DISTRIC_OK;

    if ((err = tlv_encode_uint8(enc,  CAP_TAG_MAJOR,   cap->major_version)) != DISTRIC_OK) goto fail;
    if ((err = tlv_encode_uint8(enc,  CAP_TAG_MINOR,   cap->minor_version)) != DISTRIC_OK) goto fail;
    if ((err = tlv_encode_uint32(enc, CAP_TAG_CAPS,    cap->capabilities))  != DISTRIC_OK) goto fail;
    if (cap->node_id[0] != '\0') {
        if ((err = tlv_encode_string(enc, CAP_TAG_NODE_ID, cap->node_id)) != DISTRIC_OK) goto fail;
    }

    *buf_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    return *buf_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;

fail:
    tlv_encoder_free(enc);
    return err;
}

distric_err_t capability_deserialize(
    const uint8_t  *buf,
    size_t          len,
    capability_msg_t *cap_out)
{
    if (!buf || !len || !cap_out) return DISTRIC_ERR_INVALID_ARG;
    memset(cap_out, 0, sizeof(*cap_out));

    tlv_decoder_t *dec = tlv_decoder_create(buf, len);
    if (!dec) return DISTRIC_ERR_NO_MEMORY;

    uint32_t seen = 0;
    tlv_field_t field;
    distric_err_t err;

    while ((err = tlv_decode_next(dec, &field)) == DISTRIC_OK) {
        switch (field.tag) {
        case CAP_TAG_MAJOR:
            cap_out->major_version = tlv_field_get_uint8(&field);
            seen |= 1u;
            break;
        case CAP_TAG_MINOR:
            cap_out->minor_version = tlv_field_get_uint8(&field);
            seen |= 2u;
            break;
        case CAP_TAG_CAPS:
            cap_out->capabilities = tlv_field_get_uint32(&field);
            seen |= 4u;
            break;
        case CAP_TAG_NODE_ID: {
            const char *s = tlv_field_get_string(&field);
            if (s) {
                size_t slen = strlen(s);
                if (slen >= sizeof(cap_out->node_id)) slen = sizeof(cap_out->node_id)-1u;
                memcpy(cap_out->node_id, s, slen);
                cap_out->node_id[slen] = '\0';
            }
            break;
        }
        default:
            break;
        }
    }

    tlv_decoder_free(dec);

    if ((seen & 7u) != 7u) {
        distric_set_error_info(DISTRIC_ERR_INVALID_FORMAT,
                               PROTO_STAGE_CAPABILITY_NEGOTIATE, 0, 0,
                               "capability message missing required fields");
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    return DISTRIC_OK;
}

distric_err_t capability_negotiate(
    const capability_msg_t *local,
    const capability_msg_t *peer,
    uint32_t               *negotiated_caps_out)
{
    if (!local || !peer || !negotiated_caps_out) return DISTRIC_ERR_INVALID_ARG;

    if (local->major_version != peer->major_version) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "major version mismatch: local=%u peer=%u",
                 local->major_version, peer->major_version);
        distric_set_error_info(DISTRIC_ERR_INVALID_FORMAT,
                               PROTO_STAGE_CAPABILITY_NEGOTIATE, 0, 0, detail);
        return DISTRIC_ERR_INVALID_FORMAT;
    }

    *negotiated_caps_out = local->capabilities & peer->capabilities;
    return DISTRIC_OK;
}