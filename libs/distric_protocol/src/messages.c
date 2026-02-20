/**
 * @file messages.c
 * @brief Protocol Message Serialization/Deserialization Implementation
 *
 * Fix #3 — Required-field validation:
 *   All deserialize_*() functions previously returned DISTRIC_OK even when
 *   mandatory fields (e.g., candidate_id in RequestVote, task_id in
 *   TaskAssignment) were absent from the TLV payload.  A truncated or
 *   adversarially crafted message was indistinguishable from a valid one
 *   with empty strings and zero numerics.
 *
 *   Each deserializer now maintains a `uint32_t seen` bitmask.  Each decoded
 *   field sets its corresponding bit.  After the decode loop the function
 *   checks that all required bits are set, returning DISTRIC_ERR_INVALID_FORMAT
 *   (re-used; consumers should treat this as "message malformed / missing field")
 *   on failure.  Optional fields — those that have sensible zero/NULL defaults —
 *   are deliberately NOT required.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "distric_protocol/messages.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * REQUIRED-FIELD BITMASK HELPERS
 * Each message defines a set of bit constants and a REQUIRED mask.
 * Bits are assigned in the order fields appear in the struct (0 = bit 0 etc.).
 * ========================================================================= */

/* Return DISTRIC_ERR_INVALID_FORMAT and free decoder if required bits missing */
#define CHECK_REQUIRED(dec, seen, required)                         \
    do {                                                             \
        if (((seen) & (required)) != (required)) {                   \
            tlv_decoder_free(dec);                                   \
            return DISTRIC_ERR_INVALID_FORMAT;                       \
        }                                                            \
    } while (0)

/* ============================================================================
 * RAFT — REQUEST VOTE
 * Required: term, candidate_id, last_log_index, last_log_term
 * ========================================================================= */
#define RV_SEEN_TERM            (1u << 0)
#define RV_SEEN_CANDIDATE_ID    (1u << 1)
#define RV_SEEN_LAST_LOG_INDEX  (1u << 2)
#define RV_SEEN_LAST_LOG_TERM   (1u << 3)
#define RV_REQUIRED             (RV_SEEN_TERM | RV_SEEN_CANDIDATE_ID | \
                                 RV_SEEN_LAST_LOG_INDEX | RV_SEEN_LAST_LOG_TERM)

distric_err_t serialize_raft_request_vote(
    const raft_request_vote_t* msg,
    uint8_t** buffer_out,
    size_t*   len_out)
{
    if (!msg || !buffer_out || !len_out) return DISTRIC_ERR_INVALID_ARG;

    tlv_encoder_t* enc = tlv_encoder_create(256);
    if (!enc) return DISTRIC_ERR_NO_MEMORY;

    tlv_encode_uint32(enc, FIELD_TERM,           msg->term);
    tlv_encode_string(enc, FIELD_CANDIDATE_ID,   msg->candidate_id);
    tlv_encode_uint32(enc, FIELD_LAST_LOG_INDEX, msg->last_log_index);
    tlv_encode_uint32(enc, FIELD_LAST_LOG_TERM,  msg->last_log_term);

    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_raft_request_vote(
    const uint8_t*        buffer,
    size_t                len,
    raft_request_vote_t*  msg_out)
{
    if (!buffer || !msg_out || len == 0) return DISTRIC_ERR_INVALID_ARG;
    if (!tlv_validate_buffer(buffer, len)) return DISTRIC_ERR_INVALID_FORMAT;

    memset(msg_out, 0, sizeof(*msg_out));

    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) return DISTRIC_ERR_NO_MEMORY;

    uint32_t seen = 0;
    tlv_field_t field;

    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_TERM:
                tlv_field_get_uint32(&field, &msg_out->term);
                seen |= RV_SEEN_TERM;
                break;
            case FIELD_CANDIDATE_ID: {
                const char* s = tlv_field_get_string(&field);
                if (s) { strncpy(msg_out->candidate_id, s,
                                 sizeof(msg_out->candidate_id) - 1); }
                seen |= RV_SEEN_CANDIDATE_ID;
                break;
            }
            case FIELD_LAST_LOG_INDEX:
                tlv_field_get_uint32(&field, &msg_out->last_log_index);
                seen |= RV_SEEN_LAST_LOG_INDEX;
                break;
            case FIELD_LAST_LOG_TERM:
                tlv_field_get_uint32(&field, &msg_out->last_log_term);
                seen |= RV_SEEN_LAST_LOG_TERM;
                break;
            default:
                break;
        }
    }

    CHECK_REQUIRED(dec, seen, RV_REQUIRED);
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

void free_raft_request_vote(raft_request_vote_t* msg)
{
    (void)msg; /* no heap allocations */
}

/* ============================================================================
 * RAFT — REQUEST VOTE RESPONSE
 * Required: term, vote_granted, node_id
 * ========================================================================= */
#define RVR_SEEN_TERM          (1u << 0)
#define RVR_SEEN_VOTE_GRANTED  (1u << 1)
#define RVR_SEEN_NODE_ID       (1u << 2)
#define RVR_REQUIRED           (RVR_SEEN_TERM | RVR_SEEN_VOTE_GRANTED | RVR_SEEN_NODE_ID)

distric_err_t serialize_raft_request_vote_response(
    const raft_request_vote_response_t* msg,
    uint8_t** buffer_out,
    size_t*   len_out)
{
    if (!msg || !buffer_out || !len_out) return DISTRIC_ERR_INVALID_ARG;

    tlv_encoder_t* enc = tlv_encoder_create(128);
    if (!enc) return DISTRIC_ERR_NO_MEMORY;

    tlv_encode_uint32(enc, FIELD_TERM,         msg->term);
    tlv_encode_bool  (enc, FIELD_VOTE_GRANTED, msg->vote_granted);
    tlv_encode_string(enc, FIELD_NODE_ID,      msg->node_id);

    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_raft_request_vote_response(
    const uint8_t*                buffer,
    size_t                        len,
    raft_request_vote_response_t* msg_out)
{
    if (!buffer || !msg_out || len == 0) return DISTRIC_ERR_INVALID_ARG;
    if (!tlv_validate_buffer(buffer, len)) return DISTRIC_ERR_INVALID_FORMAT;

    memset(msg_out, 0, sizeof(*msg_out));

    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) return DISTRIC_ERR_NO_MEMORY;

    uint32_t seen = 0;
    tlv_field_t field;

    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_TERM:
                tlv_field_get_uint32(&field, &msg_out->term);
                seen |= RVR_SEEN_TERM;
                break;
            case FIELD_VOTE_GRANTED:
                tlv_field_get_bool(&field, &msg_out->vote_granted);
                seen |= RVR_SEEN_VOTE_GRANTED;
                break;
            case FIELD_NODE_ID: {
                const char* s = tlv_field_get_string(&field);
                if (s) { strncpy(msg_out->node_id, s,
                                 sizeof(msg_out->node_id) - 1); }
                seen |= RVR_SEEN_NODE_ID;
                break;
            }
            default:
                break;
        }
    }

    CHECK_REQUIRED(dec, seen, RVR_REQUIRED);
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

/* ============================================================================
 * RAFT — APPEND ENTRIES
 * Required: term, leader_id, prev_log_index, prev_log_term, leader_commit
 * Optional: entries (may be empty heartbeat)
 * ========================================================================= */
#define AE_SEEN_TERM            (1u << 0)
#define AE_SEEN_LEADER_ID       (1u << 1)
#define AE_SEEN_PREV_LOG_INDEX  (1u << 2)
#define AE_SEEN_PREV_LOG_TERM   (1u << 3)
#define AE_SEEN_LEADER_COMMIT   (1u << 4)
#define AE_REQUIRED             (AE_SEEN_TERM | AE_SEEN_LEADER_ID | \
                                 AE_SEEN_PREV_LOG_INDEX | AE_SEEN_PREV_LOG_TERM | \
                                 AE_SEEN_LEADER_COMMIT)

distric_err_t serialize_raft_append_entries(
    const raft_append_entries_t* msg,
    uint8_t** buffer_out,
    size_t*   len_out)
{
    if (!msg || !buffer_out || !len_out) return DISTRIC_ERR_INVALID_ARG;

    tlv_encoder_t* enc = tlv_encoder_create(512);
    if (!enc) return DISTRIC_ERR_NO_MEMORY;

    tlv_encode_uint32(enc, FIELD_TERM,           msg->term);
    tlv_encode_string(enc, FIELD_LEADER_ID,      msg->leader_id);
    tlv_encode_uint32(enc, FIELD_PREV_LOG_INDEX, msg->prev_log_index);
    tlv_encode_uint32(enc, FIELD_PREV_LOG_TERM,  msg->prev_log_term);
    tlv_encode_uint32(enc, FIELD_LEADER_COMMIT,  msg->leader_commit);

    /* Encode entries — each as a nested TLV bytes blob */
    for (size_t i = 0; i < msg->entry_count; i++) {
        const raft_log_entry_wire_t* e = &msg->entries[i];
        tlv_encoder_t* entry_enc = tlv_encoder_create(128);
        if (!entry_enc) { tlv_encoder_free(enc); return DISTRIC_ERR_NO_MEMORY; }

        tlv_encode_uint32(entry_enc, FIELD_ENTRY_INDEX, e->index);
        tlv_encode_uint32(entry_enc, FIELD_ENTRY_TERM,  e->term);
        tlv_encode_uint8 (entry_enc, FIELD_ENTRY_TYPE,  e->entry_type);
        if (e->data && e->data_len > 0) {
            tlv_encode_bytes(entry_enc, FIELD_ENTRY_DATA, e->data, e->data_len);
        }

        size_t   entry_len;
        uint8_t* entry_buf = tlv_encoder_detach(entry_enc, &entry_len);
        tlv_encoder_free(entry_enc);
        if (!entry_buf) { tlv_encoder_free(enc); return DISTRIC_ERR_NO_MEMORY; }

        tlv_encode_bytes(enc, FIELD_ENTRIES, entry_buf, entry_len);
        free(entry_buf);
    }

    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_raft_append_entries(
    const uint8_t*        buffer,
    size_t                len,
    raft_append_entries_t* msg_out)
{
    if (!buffer || !msg_out || len == 0) return DISTRIC_ERR_INVALID_ARG;
    if (!tlv_validate_buffer(buffer, len)) return DISTRIC_ERR_INVALID_FORMAT;

    memset(msg_out, 0, sizeof(*msg_out));

    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) return DISTRIC_ERR_NO_MEMORY;

    uint32_t seen = 0;
    tlv_field_t field;

    /* First pass: count entries */
    size_t entry_count = 0;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        if (field.tag == FIELD_ENTRIES) entry_count++;
    }

    if (entry_count > 0) {
        msg_out->entries = (raft_log_entry_wire_t*)calloc(
                               entry_count, sizeof(raft_log_entry_wire_t));
        if (!msg_out->entries) {
            tlv_decoder_free(dec);
            return DISTRIC_ERR_NO_MEMORY;
        }
    }

    /* Second pass: decode */
    tlv_decoder_reset(dec);
    size_t entry_idx = 0;

    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_TERM:
                tlv_field_get_uint32(&field, &msg_out->term);
                seen |= AE_SEEN_TERM;
                break;
            case FIELD_LEADER_ID: {
                const char* s = tlv_field_get_string(&field);
                if (s) { strncpy(msg_out->leader_id, s,
                                 sizeof(msg_out->leader_id) - 1); }
                seen |= AE_SEEN_LEADER_ID;
                break;
            }
            case FIELD_PREV_LOG_INDEX:
                tlv_field_get_uint32(&field, &msg_out->prev_log_index);
                seen |= AE_SEEN_PREV_LOG_INDEX;
                break;
            case FIELD_PREV_LOG_TERM:
                tlv_field_get_uint32(&field, &msg_out->prev_log_term);
                seen |= AE_SEEN_PREV_LOG_TERM;
                break;
            case FIELD_LEADER_COMMIT:
                tlv_field_get_uint32(&field, &msg_out->leader_commit);
                seen |= AE_SEEN_LEADER_COMMIT;
                break;
            case FIELD_ENTRIES: {
                if (entry_idx >= entry_count) break;
                raft_log_entry_wire_t* e = &msg_out->entries[entry_idx++];

                tlv_decoder_t* nd = tlv_decoder_create(field.value, field.length);
                if (nd) {
                    tlv_field_t nf;
                    while (tlv_decode_next(nd, &nf) == DISTRIC_OK) {
                        switch (nf.tag) {
                            case FIELD_ENTRY_INDEX:
                                tlv_field_get_uint32(&nf, &e->index); break;
                            case FIELD_ENTRY_TERM:
                                tlv_field_get_uint32(&nf, &e->term);  break;
                            case FIELD_ENTRY_TYPE:
                                tlv_field_get_uint8(&nf, &e->entry_type); break;
                            case FIELD_ENTRY_DATA: {
                                size_t dlen;
                                const uint8_t* d = tlv_field_get_bytes(&nf, &dlen);
                                if (d && dlen > 0) {
                                    e->data = (uint8_t*)malloc(dlen);
                                    if (e->data) {
                                        memcpy(e->data, d, dlen);
                                        e->data_len = dlen;
                                    }
                                }
                                break;
                            }
                            default: break;
                        }
                    }
                    tlv_decoder_free(nd);
                }
                break;
            }
            default:
                break;
        }
    }

    msg_out->entry_count = entry_idx;

    CHECK_REQUIRED(dec, seen, AE_REQUIRED);
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

distric_err_t deserialize_raft_append_entries_response(
    const uint8_t*                    buffer,
    size_t                            len,
    raft_append_entries_response_t*   msg_out)
{
    if (!buffer || !msg_out || len == 0) return DISTRIC_ERR_INVALID_ARG;
    if (!tlv_validate_buffer(buffer, len)) return DISTRIC_ERR_INVALID_FORMAT;

    memset(msg_out, 0, sizeof(*msg_out));

    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) return DISTRIC_ERR_NO_MEMORY;

    /* Required: term, success, node_id */
    uint32_t seen = 0;
#define AER_SEEN_TERM    (1u << 0)
#define AER_SEEN_SUCCESS (1u << 1)
#define AER_SEEN_NODE_ID (1u << 2)
#define AER_REQUIRED     (AER_SEEN_TERM | AER_SEEN_SUCCESS | AER_SEEN_NODE_ID)

    tlv_field_t field;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_TERM:
                tlv_field_get_uint32(&field, &msg_out->term);
                seen |= AER_SEEN_TERM;
                break;
            case FIELD_SUCCESS:
                tlv_field_get_bool(&field, &msg_out->success);
                seen |= AER_SEEN_SUCCESS;
                break;
            case FIELD_NODE_ID: {
                const char* s = tlv_field_get_string(&field);
                if (s) { strncpy(msg_out->node_id, s,
                                 sizeof(msg_out->node_id) - 1); }
                seen |= AER_SEEN_NODE_ID;
                break;
            }
            case FIELD_MATCH_INDEX:
                tlv_field_get_uint32(&field, &msg_out->match_index);
                break;
            default:
                break;
        }
    }

    CHECK_REQUIRED(dec, seen, AER_REQUIRED);
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

distric_err_t serialize_raft_append_entries_response(
    const raft_append_entries_response_t* msg,
    uint8_t** buffer_out,
    size_t*   len_out)
{
    if (!msg || !buffer_out || !len_out) return DISTRIC_ERR_INVALID_ARG;

    tlv_encoder_t* enc = tlv_encoder_create(128);
    if (!enc) return DISTRIC_ERR_NO_MEMORY;

    tlv_encode_uint32(enc, FIELD_TERM,        msg->term);
    tlv_encode_bool  (enc, FIELD_SUCCESS,     msg->success);
    tlv_encode_string(enc, FIELD_NODE_ID,     msg->node_id);
    tlv_encode_uint32(enc, FIELD_MATCH_INDEX, msg->match_index);

    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

void free_raft_append_entries(raft_append_entries_t* msg)
{
    if (!msg) return;
    if (msg->entries) {
        for (size_t i = 0; i < msg->entry_count; i++) {
            free(msg->entries[i].data);
        }
        free(msg->entries);
        msg->entries     = NULL;
        msg->entry_count = 0;
    }
}

/* ============================================================================
 * RAFT — INSTALL SNAPSHOT (abbreviated — same pattern)
 * Required: term, leader_id, last_included_index, last_included_term
 * ========================================================================= */
distric_err_t serialize_raft_install_snapshot(
    const raft_install_snapshot_t* msg,
    uint8_t** buffer_out,
    size_t*   len_out)
{
    if (!msg || !buffer_out || !len_out) return DISTRIC_ERR_INVALID_ARG;

    tlv_encoder_t* enc = tlv_encoder_create(1024);
    if (!enc) return DISTRIC_ERR_NO_MEMORY;

    tlv_encode_uint32(enc, FIELD_TERM,            msg->term);
    tlv_encode_string(enc, FIELD_LEADER_ID,       msg->leader_id);
    tlv_encode_uint32(enc, FIELD_SNAPSHOT_INDEX,  msg->last_included_index);
    tlv_encode_uint32(enc, FIELD_SNAPSHOT_TERM,   msg->last_included_term);
    if (msg->data && msg->data_len > 0) {
        tlv_encode_bytes(enc, FIELD_SNAPSHOT_DATA, msg->data, msg->data_len);
    }

    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_raft_install_snapshot(
    const uint8_t*          buffer,
    size_t                  len,
    raft_install_snapshot_t* msg_out)
{
    if (!buffer || !msg_out || len == 0) return DISTRIC_ERR_INVALID_ARG;
    if (!tlv_validate_buffer(buffer, len)) return DISTRIC_ERR_INVALID_FORMAT;

    memset(msg_out, 0, sizeof(*msg_out));

    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) return DISTRIC_ERR_NO_MEMORY;

    uint32_t seen = 0;
#define IS_SEEN_TERM  (1u << 0)
#define IS_SEEN_LID   (1u << 1)
#define IS_SEEN_IDX   (1u << 2)
#define IS_SEEN_TERM2 (1u << 3)
#define IS_REQUIRED   (IS_SEEN_TERM | IS_SEEN_LID | IS_SEEN_IDX | IS_SEEN_TERM2)

    tlv_field_t field;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_TERM:
                tlv_field_get_uint32(&field, &msg_out->term);
                seen |= IS_SEEN_TERM;
                break;
            case FIELD_LEADER_ID: {
                const char* s = tlv_field_get_string(&field);
                if (s) { strncpy(msg_out->leader_id, s,
                                 sizeof(msg_out->leader_id) - 1); }
                seen |= IS_SEEN_LID;
                break;
            }
            case FIELD_SNAPSHOT_INDEX:
                tlv_field_get_uint32(&field, &msg_out->last_included_index);
                seen |= IS_SEEN_IDX;
                break;
            case FIELD_SNAPSHOT_TERM:
                tlv_field_get_uint32(&field, &msg_out->last_included_term);
                seen |= IS_SEEN_TERM2;
                break;
            case FIELD_SNAPSHOT_DATA: {
                size_t dlen;
                const uint8_t* d = tlv_field_get_bytes(&field, &dlen);
                if (d && dlen > 0) {
                    msg_out->data = (uint8_t*)malloc(dlen);
                    if (msg_out->data) {
                        memcpy(msg_out->data, d, dlen);
                        msg_out->data_len = dlen;
                    }
                }
                break;
            }
            default: break;
        }
    }

    CHECK_REQUIRED(dec, seen, IS_REQUIRED);
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

distric_err_t serialize_raft_install_snapshot_response(
    const raft_install_snapshot_response_t* msg,
    uint8_t** buffer_out,
    size_t*   len_out)
{
    if (!msg || !buffer_out || !len_out) return DISTRIC_ERR_INVALID_ARG;

    tlv_encoder_t* enc = tlv_encoder_create(128);
    if (!enc) return DISTRIC_ERR_NO_MEMORY;

    tlv_encode_uint32(enc, FIELD_TERM,    msg->term);
    tlv_encode_string(enc, FIELD_NODE_ID, msg->node_id);
    tlv_encode_bool  (enc, FIELD_SUCCESS, msg->success);

    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_raft_install_snapshot_response(
    const uint8_t*                    buffer,
    size_t                            len,
    raft_install_snapshot_response_t* msg_out)
{
    if (!buffer || !msg_out || len == 0) return DISTRIC_ERR_INVALID_ARG;
    if (!tlv_validate_buffer(buffer, len)) return DISTRIC_ERR_INVALID_FORMAT;

    memset(msg_out, 0, sizeof(*msg_out));

    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) return DISTRIC_ERR_NO_MEMORY;

    uint32_t seen = 0;
#define ISR_SEEN_TERM    (1u << 0)
#define ISR_SEEN_NODE_ID (1u << 1)
#define ISR_SEEN_SUCCESS (1u << 2)
#define ISR_REQUIRED     (ISR_SEEN_TERM | ISR_SEEN_NODE_ID | ISR_SEEN_SUCCESS)

    tlv_field_t field;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_TERM:
                tlv_field_get_uint32(&field, &msg_out->term);
                seen |= ISR_SEEN_TERM;
                break;
            case FIELD_NODE_ID: {
                const char* s = tlv_field_get_string(&field);
                if (s) { strncpy(msg_out->node_id, s,
                                 sizeof(msg_out->node_id) - 1); }
                seen |= ISR_SEEN_NODE_ID;
                break;
            }
            case FIELD_SUCCESS:
                tlv_field_get_bool(&field, &msg_out->success);
                seen |= ISR_SEEN_SUCCESS;
                break;
            default: break;
        }
    }

    CHECK_REQUIRED(dec, seen, ISR_REQUIRED);
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

void free_raft_install_snapshot(raft_install_snapshot_t* msg)
{
    if (!msg) return;
    free(msg->data);
    msg->data     = NULL;
    msg->data_len = 0;
}

/* ============================================================================
 * GOSSIP — PING
 * Required: sender_id, incarnation, sequence_number
 * ========================================================================= */
#define GP_SEEN_SENDER_ID       (1u << 0)
#define GP_SEEN_INCARNATION     (1u << 1)
#define GP_SEEN_SEQ             (1u << 2)
#define GP_REQUIRED             (GP_SEEN_SENDER_ID | GP_SEEN_INCARNATION | GP_SEEN_SEQ)

distric_err_t serialize_gossip_ping(
    const gossip_ping_t* msg,
    uint8_t** buffer_out,
    size_t*   len_out)
{
    if (!msg || !buffer_out || !len_out) return DISTRIC_ERR_INVALID_ARG;

    tlv_encoder_t* enc = tlv_encoder_create(128);
    if (!enc) return DISTRIC_ERR_NO_MEMORY;

    tlv_encode_string(enc, FIELD_NODE_ID,        msg->sender_id);
    tlv_encode_uint64(enc, FIELD_INCARNATION,    msg->incarnation);
    tlv_encode_uint32(enc, FIELD_SEQUENCE_NUMBER,msg->sequence_number);

    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_gossip_ping(
    const uint8_t* buffer,
    size_t         len,
    gossip_ping_t* msg_out)
{
    if (!buffer || !msg_out || len == 0) return DISTRIC_ERR_INVALID_ARG;
    if (!tlv_validate_buffer(buffer, len)) return DISTRIC_ERR_INVALID_FORMAT;

    memset(msg_out, 0, sizeof(*msg_out));

    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) return DISTRIC_ERR_NO_MEMORY;

    uint32_t seen = 0;
    tlv_field_t field;

    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_NODE_ID: {
                const char* s = tlv_field_get_string(&field);
                if (s) { strncpy(msg_out->sender_id, s,
                                 sizeof(msg_out->sender_id) - 1); }
                seen |= GP_SEEN_SENDER_ID;
                break;
            }
            case FIELD_INCARNATION:
                tlv_field_get_uint64(&field, &msg_out->incarnation);
                seen |= GP_SEEN_INCARNATION;
                break;
            case FIELD_SEQUENCE_NUMBER:
                tlv_field_get_uint32(&field, &msg_out->sequence_number);
                seen |= GP_SEEN_SEQ;
                break;
            default: break;
        }
    }

    CHECK_REQUIRED(dec, seen, GP_REQUIRED);
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

/* ============================================================================
 * GOSSIP — ACK (same fields as Ping)
 * ========================================================================= */
distric_err_t serialize_gossip_ack(
    const gossip_ack_t* msg,
    uint8_t** buffer_out,
    size_t*   len_out)
{
    if (!msg || !buffer_out || !len_out) return DISTRIC_ERR_INVALID_ARG;

    tlv_encoder_t* enc = tlv_encoder_create(128);
    if (!enc) return DISTRIC_ERR_NO_MEMORY;

    tlv_encode_string(enc, FIELD_NODE_ID,         msg->sender_id);
    tlv_encode_uint64(enc, FIELD_INCARNATION,     msg->incarnation);
    tlv_encode_uint32(enc, FIELD_SEQUENCE_NUMBER, msg->sequence_number);

    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_gossip_ack(
    const uint8_t* buffer,
    size_t         len,
    gossip_ack_t*  msg_out)
{
    if (!buffer || !msg_out || len == 0) return DISTRIC_ERR_INVALID_ARG;
    if (!tlv_validate_buffer(buffer, len)) return DISTRIC_ERR_INVALID_FORMAT;

    memset(msg_out, 0, sizeof(*msg_out));

    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) return DISTRIC_ERR_NO_MEMORY;

    uint32_t seen = 0;
    tlv_field_t field;

    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_NODE_ID: {
                const char* s = tlv_field_get_string(&field);
                if (s) { strncpy(msg_out->sender_id, s,
                                 sizeof(msg_out->sender_id) - 1); }
                seen |= GP_SEEN_SENDER_ID;
                break;
            }
            case FIELD_INCARNATION:
                tlv_field_get_uint64(&field, &msg_out->incarnation);
                seen |= GP_SEEN_INCARNATION;
                break;
            case FIELD_SEQUENCE_NUMBER:
                tlv_field_get_uint32(&field, &msg_out->sequence_number);
                seen |= GP_SEEN_SEQ;
                break;
            default: break;
        }
    }

    CHECK_REQUIRED(dec, seen, GP_REQUIRED);
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

/* ============================================================================
 * GOSSIP — INDIRECT PING
 * Required: sender_id, target_id, sequence_number
 * ========================================================================= */
#define GIP_SEEN_SENDER  (1u << 0)
#define GIP_SEEN_TARGET  (1u << 1)
#define GIP_SEEN_SEQ     (1u << 2)
#define GIP_REQUIRED     (GIP_SEEN_SENDER | GIP_SEEN_TARGET | GIP_SEEN_SEQ)

distric_err_t serialize_gossip_indirect_ping(
    const gossip_indirect_ping_t* msg,
    uint8_t** buffer_out,
    size_t*   len_out)
{
    if (!msg || !buffer_out || !len_out) return DISTRIC_ERR_INVALID_ARG;

    tlv_encoder_t* enc = tlv_encoder_create(128);
    if (!enc) return DISTRIC_ERR_NO_MEMORY;

    tlv_encode_string(enc, FIELD_NODE_ID,         msg->sender_id);
    tlv_encode_string(enc, FIELD_TARGET_ID,       msg->target_id);
    tlv_encode_uint32(enc, FIELD_SEQUENCE_NUMBER, msg->sequence_number);

    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_gossip_indirect_ping(
    const uint8_t*         buffer,
    size_t                 len,
    gossip_indirect_ping_t* msg_out)
{
    if (!buffer || !msg_out || len == 0) return DISTRIC_ERR_INVALID_ARG;
    if (!tlv_validate_buffer(buffer, len)) return DISTRIC_ERR_INVALID_FORMAT;

    memset(msg_out, 0, sizeof(*msg_out));

    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) return DISTRIC_ERR_NO_MEMORY;

    uint32_t seen = 0;
    tlv_field_t field;

    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_NODE_ID: {
                const char* s = tlv_field_get_string(&field);
                if (s) { strncpy(msg_out->sender_id, s,
                                 sizeof(msg_out->sender_id) - 1); }
                seen |= GIP_SEEN_SENDER;
                break;
            }
            case FIELD_TARGET_ID: {
                const char* s = tlv_field_get_string(&field);
                if (s) { strncpy(msg_out->target_id, s,
                                 sizeof(msg_out->target_id) - 1); }
                seen |= GIP_SEEN_TARGET;
                break;
            }
            case FIELD_SEQUENCE_NUMBER:
                tlv_field_get_uint32(&field, &msg_out->sequence_number);
                seen |= GIP_SEEN_SEQ;
                break;
            default: break;
        }
    }

    CHECK_REQUIRED(dec, seen, GIP_REQUIRED);
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

/* ============================================================================
 * GOSSIP — MEMBERSHIP UPDATE
 * Required: sender_id
 * Optional: updates[] (may be empty)
 * ========================================================================= */
distric_err_t serialize_gossip_membership_update(
    const gossip_membership_update_t* msg,
    uint8_t** buffer_out,
    size_t*   len_out)
{
    if (!msg || !buffer_out || !len_out) return DISTRIC_ERR_INVALID_ARG;

    tlv_encoder_t* enc = tlv_encoder_create(512);
    if (!enc) return DISTRIC_ERR_NO_MEMORY;

    tlv_encode_string(enc, FIELD_NODE_ID, msg->sender_id);

    for (size_t i = 0; i < msg->update_count; i++) {
        const gossip_node_info_wire_t* n = &msg->updates[i];
        tlv_encoder_t* ne = tlv_encoder_create(256);
        if (!ne) { tlv_encoder_free(enc); return DISTRIC_ERR_NO_MEMORY; }

        tlv_encode_string(ne, FIELD_NODE_ID,      n->node_id);
        tlv_encode_string(ne, FIELD_NODE_ADDRESS, n->address);
        tlv_encode_uint16(ne, FIELD_NODE_PORT,    n->port);
        tlv_encode_uint32(ne, FIELD_NODE_STATE,   (uint32_t)n->state);
        tlv_encode_uint32(ne, FIELD_NODE_ROLE,    (uint32_t)n->role);
        tlv_encode_uint64(ne, FIELD_INCARNATION,  n->incarnation);
        tlv_encode_uint8 (ne, FIELD_CPU_USAGE,    n->cpu_usage);
        tlv_encode_uint8 (ne, FIELD_MEMORY_USAGE, n->memory_usage);

        size_t   nb_len;
        uint8_t* nb = tlv_encoder_detach(ne, &nb_len);
        tlv_encoder_free(ne);
        if (!nb) { tlv_encoder_free(enc); return DISTRIC_ERR_NO_MEMORY; }
        tlv_encode_bytes(enc, FIELD_NODE_INFO, nb, nb_len);
        free(nb);
    }

    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_gossip_membership_update(
    const uint8_t*              buffer,
    size_t                      len,
    gossip_membership_update_t* msg_out)
{
    if (!buffer || !msg_out || len == 0) return DISTRIC_ERR_INVALID_ARG;
    if (!tlv_validate_buffer(buffer, len)) return DISTRIC_ERR_INVALID_FORMAT;

    memset(msg_out, 0, sizeof(*msg_out));

    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) return DISTRIC_ERR_NO_MEMORY;

    uint32_t seen = 0;
#define GMU_SEEN_SENDER_ID (1u << 0)
#define GMU_REQUIRED       GMU_SEEN_SENDER_ID

    /* Count nodes first */
    size_t node_count = 0;
    tlv_field_t field;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        if (field.tag == FIELD_NODE_INFO) node_count++;
    }

    if (node_count > 0) {
        msg_out->updates = (gossip_node_info_wire_t*)calloc(
                               node_count, sizeof(gossip_node_info_wire_t));
        if (!msg_out->updates) {
            tlv_decoder_free(dec);
            return DISTRIC_ERR_NO_MEMORY;
        }
    }

    tlv_decoder_reset(dec);
    size_t node_idx = 0;

    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_NODE_ID: {
                const char* s = tlv_field_get_string(&field);
                if (s) { strncpy(msg_out->sender_id, s,
                                 sizeof(msg_out->sender_id) - 1); }
                seen |= GMU_SEEN_SENDER_ID;
                break;
            }
            case FIELD_NODE_INFO: {
                if (node_idx >= node_count) break;
                gossip_node_info_wire_t* node = &msg_out->updates[node_idx++];

                tlv_decoder_t* nd = tlv_decoder_create(field.value, field.length);
                if (nd) {
                    tlv_field_t nf;
                    while (tlv_decode_next(nd, &nf) == DISTRIC_OK) {
                        switch (nf.tag) {
                            case FIELD_NODE_ID: {
                                const char* s = tlv_field_get_string(&nf);
                                if (s) strncpy(node->node_id, s,
                                               sizeof(node->node_id) - 1);
                                break;
                            }
                            case FIELD_NODE_ADDRESS: {
                                const char* s = tlv_field_get_string(&nf);
                                if (s) strncpy(node->address, s,
                                               sizeof(node->address) - 1);
                                break;
                            }
                            case FIELD_NODE_PORT:
                                tlv_field_get_uint16(&nf, &node->port); break;
                            case FIELD_NODE_STATE: {
                                uint32_t v;
                                tlv_field_get_uint32(&nf, &v);
                                node->state = (node_state_t)v;
                                break;
                            }
                            case FIELD_NODE_ROLE: {
                                uint32_t v;
                                tlv_field_get_uint32(&nf, &v);
                                node->role = (node_role_t)v;
                                break;
                            }
                            case FIELD_INCARNATION:
                                tlv_field_get_uint64(&nf, &node->incarnation); break;
                            case FIELD_CPU_USAGE:
                                tlv_field_get_uint8(&nf, &node->cpu_usage); break;
                            case FIELD_MEMORY_USAGE:
                                tlv_field_get_uint8(&nf, &node->memory_usage); break;
                            default: break;
                        }
                    }
                    tlv_decoder_free(nd);
                }
                break;
            }
            default: break;
        }
    }

    msg_out->update_count = node_idx;

    CHECK_REQUIRED(dec, seen, GMU_REQUIRED);
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

void free_gossip_membership_update(gossip_membership_update_t* msg)
{
    if (!msg) return;
    free(msg->updates);
    msg->updates      = NULL;
    msg->update_count = 0;
}

/* ============================================================================
 * TASK — ASSIGNMENT
 * Required: task_id, workflow_id, task_type, timeout_sec
 * Optional: config_json, input_data, retry_count
 * ========================================================================= */
#define TA_SEEN_TASK_ID     (1u << 0)
#define TA_SEEN_WORKFLOW_ID (1u << 1)
#define TA_SEEN_TASK_TYPE   (1u << 2)
#define TA_SEEN_TIMEOUT     (1u << 3)
#define TA_REQUIRED         (TA_SEEN_TASK_ID | TA_SEEN_WORKFLOW_ID | \
                             TA_SEEN_TASK_TYPE | TA_SEEN_TIMEOUT)

distric_err_t serialize_task_assignment(
    const task_assignment_t* msg,
    uint8_t** buffer_out,
    size_t*   len_out)
{
    if (!msg || !buffer_out || !len_out) return DISTRIC_ERR_INVALID_ARG;

    tlv_encoder_t* enc = tlv_encoder_create(512);
    if (!enc) return DISTRIC_ERR_NO_MEMORY;

    tlv_encode_string(enc, FIELD_TASK_ID,    msg->task_id);
    tlv_encode_string(enc, FIELD_WORKFLOW_ID,msg->workflow_id);
    tlv_encode_string(enc, FIELD_TASK_TYPE,  msg->task_type);
    tlv_encode_uint32(enc, FIELD_TIMEOUT_SEC,msg->timeout_sec);
    tlv_encode_uint32(enc, FIELD_RETRY_COUNT,msg->retry_count);

    if (msg->config_json) {
        tlv_encode_string(enc, FIELD_TASK_CONFIG, msg->config_json);
    }
    if (msg->input_data && msg->input_data_len > 0) {
        tlv_encode_bytes(enc, FIELD_TASK_INPUT, msg->input_data,
                         msg->input_data_len);
    }

    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_task_assignment(
    const uint8_t*     buffer,
    size_t             len,
    task_assignment_t* msg_out)
{
    if (!buffer || !msg_out || len == 0) return DISTRIC_ERR_INVALID_ARG;
    if (!tlv_validate_buffer(buffer, len)) return DISTRIC_ERR_INVALID_FORMAT;

    memset(msg_out, 0, sizeof(*msg_out));

    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) return DISTRIC_ERR_NO_MEMORY;

    uint32_t seen = 0;
    tlv_field_t field;

    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_TASK_ID: {
                const char* s = tlv_field_get_string(&field);
                if (s) { strncpy(msg_out->task_id, s,
                                 sizeof(msg_out->task_id) - 1); }
                seen |= TA_SEEN_TASK_ID;
                break;
            }
            case FIELD_WORKFLOW_ID: {
                const char* s = tlv_field_get_string(&field);
                if (s) { strncpy(msg_out->workflow_id, s,
                                 sizeof(msg_out->workflow_id) - 1); }
                seen |= TA_SEEN_WORKFLOW_ID;
                break;
            }
            case FIELD_TASK_TYPE: {
                const char* s = tlv_field_get_string(&field);
                if (s) { strncpy(msg_out->task_type, s,
                                 sizeof(msg_out->task_type) - 1); }
                seen |= TA_SEEN_TASK_TYPE;
                break;
            }
            case FIELD_TIMEOUT_SEC:
                tlv_field_get_uint32(&field, &msg_out->timeout_sec);
                seen |= TA_SEEN_TIMEOUT;
                break;
            case FIELD_RETRY_COUNT:
                tlv_field_get_uint32(&field, &msg_out->retry_count);
                break;
            case FIELD_TASK_CONFIG: {
                const char* s = tlv_field_get_string(&field);
                if (s) msg_out->config_json = strdup(s);
                break;
            }
            case FIELD_TASK_INPUT: {
                size_t dlen;
                const uint8_t* d = tlv_field_get_bytes(&field, &dlen);
                if (d && dlen > 0) {
                    msg_out->input_data = (uint8_t*)malloc(dlen);
                    if (msg_out->input_data) {
                        memcpy(msg_out->input_data, d, dlen);
                        msg_out->input_data_len = dlen;
                    }
                }
                break;
            }
            default: break;
        }
    }

    CHECK_REQUIRED(dec, seen, TA_REQUIRED);
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

void free_task_assignment(task_assignment_t* msg)
{
    if (!msg) return;
    free(msg->config_json);
    free(msg->input_data);
    msg->config_json    = NULL;
    msg->input_data     = NULL;
    msg->input_data_len = 0;
}

/* ============================================================================
 * TASK — RESULT
 * Required: task_id, worker_id, status
 * Optional: output_data, error_message, exit_code, timestamps
 * ========================================================================= */
#define TR_SEEN_TASK_ID   (1u << 0)
#define TR_SEEN_WORKER_ID (1u << 1)
#define TR_SEEN_STATUS    (1u << 2)
#define TR_REQUIRED       (TR_SEEN_TASK_ID | TR_SEEN_WORKER_ID | TR_SEEN_STATUS)

distric_err_t serialize_task_result(
    const task_result_t* msg,
    uint8_t** buffer_out,
    size_t*   len_out)
{
    if (!msg || !buffer_out || !len_out) return DISTRIC_ERR_INVALID_ARG;

    tlv_encoder_t* enc = tlv_encoder_create(1024);
    if (!enc) return DISTRIC_ERR_NO_MEMORY;

    tlv_encode_string(enc, FIELD_TASK_ID,    msg->task_id);
    tlv_encode_string(enc, FIELD_WORKER_ID,  msg->worker_id);
    tlv_encode_uint32(enc, FIELD_TASK_STATUS,(uint32_t)msg->status);

    if (msg->output_data && msg->output_data_len > 0) {
        tlv_encode_bytes(enc, FIELD_TASK_OUTPUT, msg->output_data,
                         msg->output_data_len);
    }
    if (msg->error_message) {
        tlv_encode_string(enc, FIELD_TASK_ERROR, msg->error_message);
    }
    tlv_encode_int32 (enc, FIELD_EXIT_CODE,    msg->exit_code);
    tlv_encode_uint64(enc, FIELD_STARTED_AT,   msg->started_at);
    tlv_encode_uint64(enc, FIELD_COMPLETED_AT, msg->completed_at);

    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_task_result(
    const uint8_t* buffer,
    size_t         len,
    task_result_t* msg_out)
{
    if (!buffer || !msg_out || len == 0) return DISTRIC_ERR_INVALID_ARG;
    if (!tlv_validate_buffer(buffer, len)) return DISTRIC_ERR_INVALID_FORMAT;

    memset(msg_out, 0, sizeof(*msg_out));

    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) return DISTRIC_ERR_NO_MEMORY;

    uint32_t seen = 0;
    tlv_field_t field;

    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_TASK_ID: {
                const char* s = tlv_field_get_string(&field);
                if (s) { strncpy(msg_out->task_id, s,
                                 sizeof(msg_out->task_id) - 1); }
                seen |= TR_SEEN_TASK_ID;
                break;
            }
            case FIELD_WORKER_ID: {
                const char* s = tlv_field_get_string(&field);
                if (s) { strncpy(msg_out->worker_id, s,
                                 sizeof(msg_out->worker_id) - 1); }
                seen |= TR_SEEN_WORKER_ID;
                break;
            }
            case FIELD_TASK_STATUS: {
                uint32_t v;
                tlv_field_get_uint32(&field, &v);
                msg_out->status = (task_status_t)v;
                seen |= TR_SEEN_STATUS;
                break;
            }
            case FIELD_TASK_OUTPUT: {
                size_t dlen;
                const uint8_t* d = tlv_field_get_bytes(&field, &dlen);
                if (d && dlen > 0) {
                    msg_out->output_data = (uint8_t*)malloc(dlen);
                    if (msg_out->output_data) {
                        memcpy(msg_out->output_data, d, dlen);
                        msg_out->output_data_len = dlen;
                    }
                }
                break;
            }
            case FIELD_TASK_ERROR: {
                const char* s = tlv_field_get_string(&field);
                if (s) msg_out->error_message = strdup(s);
                break;
            }
            case FIELD_EXIT_CODE:
                tlv_field_get_int32(&field, &msg_out->exit_code); break;
            case FIELD_STARTED_AT:
                tlv_field_get_uint64(&field, &msg_out->started_at); break;
            case FIELD_COMPLETED_AT:
                tlv_field_get_uint64(&field, &msg_out->completed_at); break;
            default: break;
        }
    }

    CHECK_REQUIRED(dec, seen, TR_REQUIRED);
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

void free_task_result(task_result_t* msg)
{
    if (!msg) return;
    free(msg->output_data);
    free(msg->error_message);
    msg->output_data      = NULL;
    msg->error_message    = NULL;
    msg->output_data_len  = 0;
}

/* ============================================================================
 * CLIENT — SUBMIT
 * Required: message_id, event_type, timestamp
 * Optional: payload_json
 * ========================================================================= */
#define CS_SEEN_MSG_ID     (1u << 0)
#define CS_SEEN_EVENT_TYPE (1u << 1)
#define CS_SEEN_TIMESTAMP  (1u << 2)
#define CS_REQUIRED        (CS_SEEN_MSG_ID | CS_SEEN_EVENT_TYPE | CS_SEEN_TIMESTAMP)

distric_err_t serialize_client_submit(
    const client_submit_t* msg,
    uint8_t** buffer_out,
    size_t*   len_out)
{
    if (!msg || !buffer_out || !len_out) return DISTRIC_ERR_INVALID_ARG;

    tlv_encoder_t* enc = tlv_encoder_create(512);
    if (!enc) return DISTRIC_ERR_NO_MEMORY;

    tlv_encode_string(enc, FIELD_MESSAGE_ID,  msg->message_id);
    tlv_encode_string(enc, FIELD_EVENT_TYPE,  msg->event_type);
    tlv_encode_uint64(enc, FIELD_TIMESTAMP,   msg->timestamp);

    if (msg->payload_json) {
        tlv_encode_string(enc, FIELD_MESSAGE_PAYLOAD, msg->payload_json);
    }

    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_client_submit(
    const uint8_t*   buffer,
    size_t           len,
    client_submit_t* msg_out)
{
    if (!buffer || !msg_out || len == 0) return DISTRIC_ERR_INVALID_ARG;
    if (!tlv_validate_buffer(buffer, len)) return DISTRIC_ERR_INVALID_FORMAT;

    memset(msg_out, 0, sizeof(*msg_out));

    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) return DISTRIC_ERR_NO_MEMORY;

    uint32_t seen = 0;
    tlv_field_t field;

    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_MESSAGE_ID: {
                const char* s = tlv_field_get_string(&field);
                if (s) { strncpy(msg_out->message_id, s,
                                 sizeof(msg_out->message_id) - 1); }
                seen |= CS_SEEN_MSG_ID;
                break;
            }
            case FIELD_EVENT_TYPE: {
                const char* s = tlv_field_get_string(&field);
                if (s) { strncpy(msg_out->event_type, s,
                                 sizeof(msg_out->event_type) - 1); }
                seen |= CS_SEEN_EVENT_TYPE;
                break;
            }
            case FIELD_TIMESTAMP:
                tlv_field_get_uint64(&field, &msg_out->timestamp);
                seen |= CS_SEEN_TIMESTAMP;
                break;
            case FIELD_MESSAGE_PAYLOAD: {
                const char* s = tlv_field_get_string(&field);
                if (s) msg_out->payload_json = strdup(s);
                break;
            }
            default: break;
        }
    }

    CHECK_REQUIRED(dec, seen, CS_REQUIRED);
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

void free_client_submit(client_submit_t* msg)
{
    if (!msg) return;
    free(msg->payload_json);
    msg->payload_json = NULL;
}

/* ============================================================================
 * CLIENT — RESPONSE
 * Required: message_id, response_code
 * Optional: response_message, workflows_triggered
 * ========================================================================= */
#define CR_SEEN_MSG_ID   (1u << 0)
#define CR_SEEN_RESP_CODE (1u << 1)
#define CR_REQUIRED      (CR_SEEN_MSG_ID | CR_SEEN_RESP_CODE)

distric_err_t serialize_client_response(
    const client_response_t* msg,
    uint8_t** buffer_out,
    size_t*   len_out)
{
    if (!msg || !buffer_out || !len_out) return DISTRIC_ERR_INVALID_ARG;

    tlv_encoder_t* enc = tlv_encoder_create(256);
    if (!enc) return DISTRIC_ERR_NO_MEMORY;

    tlv_encode_string(enc, FIELD_MESSAGE_ID,    msg->message_id);
    tlv_encode_uint32(enc, FIELD_RESPONSE_CODE, msg->response_code);

    if (msg->response_message) {
        tlv_encode_string(enc, FIELD_RESPONSE_MESSAGE, msg->response_message);
    }
    for (size_t i = 0; i < msg->workflow_count; i++) {
        if (msg->workflows_triggered[i]) {
            tlv_encode_string(enc, FIELD_TRIGGERED, msg->workflows_triggered[i]);
        }
    }

    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_client_response(
    const uint8_t*    buffer,
    size_t            len,
    client_response_t* msg_out)
{
    if (!buffer || !msg_out || len == 0) return DISTRIC_ERR_INVALID_ARG;
    if (!tlv_validate_buffer(buffer, len)) return DISTRIC_ERR_INVALID_FORMAT;

    memset(msg_out, 0, sizeof(*msg_out));

    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) return DISTRIC_ERR_NO_MEMORY;

    uint32_t seen = 0;
    tlv_field_t field;

    /* Count triggered workflows first */
    size_t wf_count = 0;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        if (field.tag == FIELD_TRIGGERED) wf_count++;
    }
    if (wf_count > 0) {
        msg_out->workflows_triggered = (char**)calloc(wf_count, sizeof(char*));
        if (!msg_out->workflows_triggered) {
            tlv_decoder_free(dec);
            return DISTRIC_ERR_NO_MEMORY;
        }
    }

    tlv_decoder_reset(dec);
    size_t wf_idx = 0;

    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_MESSAGE_ID: {
                const char* s = tlv_field_get_string(&field);
                if (s) { strncpy(msg_out->message_id, s,
                                 sizeof(msg_out->message_id) - 1); }
                seen |= CR_SEEN_MSG_ID;
                break;
            }
            case FIELD_RESPONSE_CODE:
                tlv_field_get_uint32(&field, &msg_out->response_code);
                seen |= CR_SEEN_RESP_CODE;
                break;
            case FIELD_RESPONSE_MESSAGE: {
                const char* s = tlv_field_get_string(&field);
                if (s) msg_out->response_message = strdup(s);
                break;
            }
            case FIELD_TRIGGERED: {
                if (wf_idx < wf_count) {
                    const char* s = tlv_field_get_string(&field);
                    if (s) msg_out->workflows_triggered[wf_idx++] = strdup(s);
                }
                break;
            }
            default: break;
        }
    }
    msg_out->workflow_count = wf_idx;

    CHECK_REQUIRED(dec, seen, CR_REQUIRED);
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

void free_client_response(client_response_t* msg)
{
    if (!msg) return;
    free(msg->response_message);
    for (size_t i = 0; i < msg->workflow_count; i++) {
        free(msg->workflows_triggered[i]);
    }
    free(msg->workflows_triggered);
    msg->response_message    = NULL;
    msg->workflows_triggered = NULL;
    msg->workflow_count      = 0;
}