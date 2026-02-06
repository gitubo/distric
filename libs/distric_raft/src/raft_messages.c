/**
 * @file raft_messages.c
 * @brief Raft Message Serialization Implementation
 * 
 * Uses distric_protocol's TLV encoding primitives for all serialization.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "distric_raft/raft_messages.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * REQUEST VOTE SERIALIZATION
 * ========================================================================= */

distric_err_t serialize_raft_request_vote(
    const raft_request_vote_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
) {
    if (!msg || !buffer_out || !len_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    tlv_encoder_t* enc = tlv_encoder_create(256);
    if (!enc) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_encode_uint32(enc, FIELD_TERM, msg->term);
    tlv_encode_string(enc, FIELD_CANDIDATE_ID, msg->candidate_id);
    tlv_encode_uint32(enc, FIELD_LAST_LOG_INDEX, msg->last_log_index);
    tlv_encode_uint32(enc, FIELD_LAST_LOG_TERM, msg->last_log_term);
    
    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_raft_request_vote(
    const uint8_t* buffer,
    size_t len,
    raft_request_vote_t* msg_out
) {
    if (!buffer || !msg_out || len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!tlv_validate_buffer(buffer, len)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    memset(msg_out, 0, sizeof(raft_request_vote_t));
    
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_field_t field;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_TERM:
                tlv_field_get_uint32(&field, &msg_out->term);
                break;
            
            case FIELD_CANDIDATE_ID: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    strncpy(msg_out->candidate_id, str, sizeof(msg_out->candidate_id) - 1);
                }
                break;
            }
            
            case FIELD_LAST_LOG_INDEX:
                tlv_field_get_uint32(&field, &msg_out->last_log_index);
                break;
            
            case FIELD_LAST_LOG_TERM:
                tlv_field_get_uint32(&field, &msg_out->last_log_term);
                break;
            
            default:
                break;
        }
    }
    
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

void free_raft_request_vote(raft_request_vote_t* msg) {
    (void)msg;  /* No dynamic allocation */
}

/* ============================================================================
 * REQUEST VOTE RESPONSE SERIALIZATION
 * ========================================================================= */

distric_err_t serialize_raft_request_vote_response(
    const raft_request_vote_response_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
) {
    if (!msg || !buffer_out || !len_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    tlv_encoder_t* enc = tlv_encoder_create(128);
    if (!enc) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_encode_uint32(enc, FIELD_TERM, msg->term);
    tlv_encode_bool(enc, FIELD_VOTE_GRANTED, msg->vote_granted);
    tlv_encode_string(enc, 0x0002, msg->node_id);  /* FIELD_NODE_ID from common */
    
    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_raft_request_vote_response(
    const uint8_t* buffer,
    size_t len,
    raft_request_vote_response_t* msg_out
) {
    if (!buffer || !msg_out || len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!tlv_validate_buffer(buffer, len)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    memset(msg_out, 0, sizeof(raft_request_vote_response_t));
    
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_field_t field;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_TERM:
                tlv_field_get_uint32(&field, &msg_out->term);
                break;
            
            case FIELD_VOTE_GRANTED:
                tlv_field_get_bool(&field, &msg_out->vote_granted);
                break;
            
            case 0x0002: {  /* FIELD_NODE_ID */
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    strncpy(msg_out->node_id, str, sizeof(msg_out->node_id) - 1);
                }
                break;
            }
            
            default:
                break;
        }
    }
    
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

/* ============================================================================
 * APPEND ENTRIES SERIALIZATION
 * ========================================================================= */

distric_err_t serialize_raft_append_entries(
    const raft_append_entries_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
) {
    if (!msg || !buffer_out || !len_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    tlv_encoder_t* enc = tlv_encoder_create(1024);
    if (!enc) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_encode_uint32(enc, FIELD_TERM, msg->term);
    tlv_encode_string(enc, FIELD_LEADER_ID, msg->leader_id);
    tlv_encode_uint32(enc, FIELD_PREV_LOG_INDEX, msg->prev_log_index);
    tlv_encode_uint32(enc, FIELD_PREV_LOG_TERM, msg->prev_log_term);
    tlv_encode_uint32(enc, FIELD_LEADER_COMMIT, msg->leader_commit);
    
    /* Encode log entries */
    if (msg->entries && msg->entry_count > 0) {
        for (size_t i = 0; i < msg->entry_count; i++) {
            const raft_log_entry_wire_t* entry = &msg->entries[i];
            
            tlv_encoder_t* entry_enc = tlv_encoder_create(256);
            tlv_encode_uint32(entry_enc, FIELD_ENTRY_INDEX, entry->index);
            tlv_encode_uint32(entry_enc, FIELD_ENTRY_TERM, entry->term);
            tlv_encode_uint32(entry_enc, FIELD_ENTRY_TYPE, (uint32_t)entry->entry_type);
            
            if (entry->data && entry->data_len > 0) {
                tlv_encode_bytes(entry_enc, FIELD_ENTRY_DATA, entry->data, entry->data_len);
            }
            
            size_t entry_len;
            uint8_t* entry_buf = tlv_encoder_finalize(entry_enc, &entry_len);
            tlv_encode_bytes(enc, FIELD_ENTRIES, entry_buf, entry_len);
            tlv_encoder_free(entry_enc);
        }
    }
    
    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_raft_append_entries(
    const uint8_t* buffer,
    size_t len,
    raft_append_entries_t* msg_out
) {
    if (!buffer || !msg_out || len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!tlv_validate_buffer(buffer, len)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    memset(msg_out, 0, sizeof(raft_append_entries_t));
    
    raft_log_entry_wire_t* entries = NULL;
    size_t entry_count = 0;
    size_t entry_capacity = 0;
    
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_field_t field;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_TERM:
                tlv_field_get_uint32(&field, &msg_out->term);
                break;
            
            case FIELD_LEADER_ID: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    strncpy(msg_out->leader_id, str, sizeof(msg_out->leader_id) - 1);
                }
                break;
            }
            
            case FIELD_PREV_LOG_INDEX:
                tlv_field_get_uint32(&field, &msg_out->prev_log_index);
                break;
            
            case FIELD_PREV_LOG_TERM:
                tlv_field_get_uint32(&field, &msg_out->prev_log_term);
                break;
            
            case FIELD_LEADER_COMMIT:
                tlv_field_get_uint32(&field, &msg_out->leader_commit);
                break;
            
            case FIELD_ENTRIES: {
                size_t entry_data_len;
                const uint8_t* entry_data = tlv_field_get_bytes(&field, &entry_data_len);
                
                if (entry_data) {
                    if (entry_count >= entry_capacity) {
                        entry_capacity = entry_capacity == 0 ? 4 : entry_capacity * 2;
                        raft_log_entry_wire_t* new_entries = (raft_log_entry_wire_t*)realloc(
                            entries, entry_capacity * sizeof(raft_log_entry_wire_t));
                        if (!new_entries) {
                            free(entries);
                            tlv_decoder_free(dec);
                            return DISTRIC_ERR_NO_MEMORY;
                        }
                        entries = new_entries;
                    }
                    
                    raft_log_entry_wire_t* entry = &entries[entry_count];
                    memset(entry, 0, sizeof(raft_log_entry_wire_t));
                    
                    tlv_decoder_t* entry_dec = tlv_decoder_create(entry_data, entry_data_len);
                    tlv_field_t entry_field;
                    
                    while (tlv_decode_next(entry_dec, &entry_field) == DISTRIC_OK) {
                        switch (entry_field.tag) {
                            case FIELD_ENTRY_INDEX:
                                tlv_field_get_uint32(&entry_field, &entry->index);
                                break;
                            
                            case FIELD_ENTRY_TERM:
                                tlv_field_get_uint32(&entry_field, &entry->term);
                                break;
                            
                            case FIELD_ENTRY_TYPE: {
                                uint32_t type;
                                tlv_field_get_uint32(&entry_field, &type);
                                entry->entry_type = (uint8_t)type;
                                break;
                            }
                            
                            case FIELD_ENTRY_DATA: {
                                size_t data_len;
                                const uint8_t* data = tlv_field_get_bytes(&entry_field, &data_len);
                                if (data && data_len > 0) {
                                    entry->data = (uint8_t*)malloc(data_len);
                                    if (entry->data) {
                                        memcpy(entry->data, data, data_len);
                                        entry->data_len = data_len;
                                    }
                                }
                                break;
                            }
                        }
                    }
                    
                    tlv_decoder_free(entry_dec);
                    entry_count++;
                }
                break;
            }
            
            default:
                break;
        }
    }
    
    msg_out->entries = entries;
    msg_out->entry_count = entry_count;
    
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

void free_raft_append_entries(raft_append_entries_t* msg) {
    if (!msg) return;
    
    if (msg->entries) {
        for (size_t i = 0; i < msg->entry_count; i++) {
            free(msg->entries[i].data);
        }
        free(msg->entries);
        msg->entries = NULL;
    }
    msg->entry_count = 0;
}

/* ============================================================================
 * APPEND ENTRIES RESPONSE SERIALIZATION
 * ========================================================================= */

distric_err_t serialize_raft_append_entries_response(
    const raft_append_entries_response_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
) {
    if (!msg || !buffer_out || !len_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    tlv_encoder_t* enc = tlv_encoder_create(128);
    if (!enc) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_encode_uint32(enc, FIELD_TERM, msg->term);
    tlv_encode_bool(enc, FIELD_SUCCESS, msg->success);
    tlv_encode_string(enc, 0x0002, msg->node_id);
    tlv_encode_uint32(enc, FIELD_LAST_LOG_INDEX, msg->match_index);
    
    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_raft_append_entries_response(
    const uint8_t* buffer,
    size_t len,
    raft_append_entries_response_t* msg_out
) {
    if (!buffer || !msg_out || len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!tlv_validate_buffer(buffer, len)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    memset(msg_out, 0, sizeof(raft_append_entries_response_t));
    
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_field_t field;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_TERM:
                tlv_field_get_uint32(&field, &msg_out->term);
                break;
            
            case FIELD_SUCCESS:
                tlv_field_get_bool(&field, &msg_out->success);
                break;
            
            case 0x0002: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    strncpy(msg_out->node_id, str, sizeof(msg_out->node_id) - 1);
                }
                break;
            }
            
            case FIELD_LAST_LOG_INDEX:
                tlv_field_get_uint32(&field, &msg_out->match_index);
                break;
            
            default:
                break;
        }
    }
    
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

/* ============================================================================
 * INSTALL SNAPSHOT SERIALIZATION
 * ========================================================================= */

distric_err_t serialize_raft_install_snapshot(
    const raft_install_snapshot_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
) {
    if (!msg || !buffer_out || !len_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    tlv_encoder_t* enc = tlv_encoder_create(1024 + msg->data_len);
    if (!enc) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_encode_uint32(enc, FIELD_TERM, msg->term);
    tlv_encode_string(enc, FIELD_LEADER_ID, msg->leader_id);
    tlv_encode_uint32(enc, FIELD_SNAPSHOT_INDEX, msg->last_included_index);
    tlv_encode_uint32(enc, FIELD_SNAPSHOT_TERM, msg->last_included_term);
    
    if (msg->data && msg->data_len > 0) {
        tlv_encode_bytes(enc, FIELD_SNAPSHOT_DATA, msg->data, msg->data_len);
    }
    
    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_raft_install_snapshot(
    const uint8_t* buffer,
    size_t len,
    raft_install_snapshot_t* msg_out
) {
    if (!buffer || !msg_out || len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!tlv_validate_buffer(buffer, len)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    memset(msg_out, 0, sizeof(raft_install_snapshot_t));
    
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_field_t field;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_TERM:
                tlv_field_get_uint32(&field, &msg_out->term);
                break;
            
            case FIELD_LEADER_ID: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    strncpy(msg_out->leader_id, str, sizeof(msg_out->leader_id) - 1);
                }
                break;
            }
            
            case FIELD_SNAPSHOT_INDEX:
                tlv_field_get_uint32(&field, &msg_out->last_included_index);
                break;
            
            case FIELD_SNAPSHOT_TERM:
                tlv_field_get_uint32(&field, &msg_out->last_included_term);
                break;
            
            case FIELD_SNAPSHOT_DATA: {
                size_t data_len;
                const uint8_t* data = tlv_field_get_bytes(&field, &data_len);
                if (data && data_len > 0) {
                    msg_out->data = (uint8_t*)malloc(data_len);
                    if (msg_out->data) {
                        memcpy(msg_out->data, data, data_len);
                        msg_out->data_len = data_len;
                    }
                }
                break;
            }
            
            default:
                break;
        }
    }
    
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

void free_raft_install_snapshot(raft_install_snapshot_t* msg) {
    if (!msg) return;
    free(msg->data);
    msg->data = NULL;
    msg->data_len = 0;
}

/* ============================================================================
 * INSTALL SNAPSHOT RESPONSE SERIALIZATION
 * ========================================================================= */

distric_err_t serialize_raft_install_snapshot_response(
    const raft_install_snapshot_response_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
) {
    if (!msg || !buffer_out || !len_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    tlv_encoder_t* enc = tlv_encoder_create(128);
    if (!enc) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_encode_uint32(enc, FIELD_TERM, msg->term);
    tlv_encode_string(enc, 0x0002, msg->node_id);
    tlv_encode_bool(enc, FIELD_SUCCESS, msg->success);
    
    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_raft_install_snapshot_response(
    const uint8_t* buffer,
    size_t len,
    raft_install_snapshot_response_t* msg_out
) {
    if (!buffer || !msg_out || len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!tlv_validate_buffer(buffer, len)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    memset(msg_out, 0, sizeof(raft_install_snapshot_response_t));
    
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_field_t field;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_TERM:
                tlv_field_get_uint32(&field, &msg_out->term);
                break;
            
            case 0x0002: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    strncpy(msg_out->node_id, str, sizeof(msg_out->node_id) - 1);
                }
                break;
            }
            
            case FIELD_SUCCESS:
                tlv_field_get_bool(&field, &msg_out->success);
                break;
            
            default:
                break;
        }
    }
    
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

/* Configuration change serialization follows same pattern... */

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

const char* raft_entry_type_to_string(uint8_t type) {
    switch (type) {
        case RAFT_ENTRY_NORMAL: return "NORMAL";
        case RAFT_ENTRY_CONFIG: return "CONFIG";
        case RAFT_ENTRY_NOOP:   return "NOOP";
        default:                return "UNKNOWN";
    }
}

const char* config_change_type_to_string(config_change_type_t type) {
    switch (type) {
        case CONFIG_CHANGE_ADD_NODE:    return "ADD_NODE";
        case CONFIG_CHANGE_REMOVE_NODE: return "REMOVE_NODE";
        case CONFIG_CHANGE_REPLACE:     return "REPLACE";
        default:                        return "UNKNOWN";
    }
}