/**
 * @file messages.c
 * @brief Protocol Message Serialization/Deserialization Implementation
 * 
 * Implements serialization and deserialization for generic protocol messages
 * using the TLV encoding system.
 * 
 * NOTE: Raft message serialization is in distric_raft library
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "distric_protocol/messages.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * GOSSIP MESSAGE SERIALIZATION - Ping
 * ========================================================================= */

distric_err_t serialize_gossip_ping(
    const gossip_ping_t* msg,
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
    
    tlv_encode_string(enc, FIELD_NODE_ID, msg->sender_id);
    tlv_encode_uint64(enc, FIELD_INCARNATION, msg->incarnation);
    tlv_encode_uint32(enc, FIELD_SEQUENCE_NUMBER, msg->sequence_number);
    
    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_gossip_ping(
    const uint8_t* buffer,
    size_t len,
    gossip_ping_t* msg_out
) {
    if (!buffer || !msg_out || len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!tlv_validate_buffer(buffer, len)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    memset(msg_out, 0, sizeof(gossip_ping_t));
    
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_field_t field;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_NODE_ID: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    strncpy(msg_out->sender_id, str, sizeof(msg_out->sender_id) - 1);
                }
                break;
            }
            
            case FIELD_INCARNATION:
                tlv_field_get_uint64(&field, &msg_out->incarnation);
                break;
            
            case FIELD_SEQUENCE_NUMBER:
                tlv_field_get_uint32(&field, &msg_out->sequence_number);
                break;
            
            default:
                break;
        }
    }
    
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

/* ============================================================================
 * GOSSIP MESSAGE SERIALIZATION - Ack
 * ========================================================================= */

distric_err_t serialize_gossip_ack(
    const gossip_ack_t* msg,
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
    
    tlv_encode_string(enc, FIELD_NODE_ID, msg->sender_id);
    tlv_encode_uint64(enc, FIELD_INCARNATION, msg->incarnation);
    tlv_encode_uint32(enc, FIELD_SEQUENCE_NUMBER, msg->sequence_number);
    
    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_gossip_ack(
    const uint8_t* buffer,
    size_t len,
    gossip_ack_t* msg_out
) {
    if (!buffer || !msg_out || len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!tlv_validate_buffer(buffer, len)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    memset(msg_out, 0, sizeof(gossip_ack_t));
    
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_field_t field;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_NODE_ID: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    strncpy(msg_out->sender_id, str, sizeof(msg_out->sender_id) - 1);
                }
                break;
            }
            
            case FIELD_INCARNATION:
                tlv_field_get_uint64(&field, &msg_out->incarnation);
                break;
            
            case FIELD_SEQUENCE_NUMBER:
                tlv_field_get_uint32(&field, &msg_out->sequence_number);
                break;
            
            default:
                break;
        }
    }
    
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

/* ============================================================================
 * GOSSIP INDIRECT PING SERIALIZATION
 * ========================================================================= */

distric_err_t serialize_gossip_indirect_ping(
    const gossip_indirect_ping_t* msg,
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
    
    tlv_encode_string(enc, FIELD_NODE_ID, msg->sender_id);
    tlv_encode_string(enc, FIELD_PING_TARGET_ID, msg->target_id);
    tlv_encode_uint32(enc, FIELD_SEQUENCE_NUMBER, msg->sequence_number);
    
    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_gossip_indirect_ping(
    const uint8_t* buffer,
    size_t len,
    gossip_indirect_ping_t* msg_out
) {
    if (!buffer || !msg_out || len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!tlv_validate_buffer(buffer, len)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    memset(msg_out, 0, sizeof(gossip_indirect_ping_t));
    
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_field_t field;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_NODE_ID: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    strncpy(msg_out->sender_id, str, sizeof(msg_out->sender_id) - 1);
                }
                break;
            }
            
            case FIELD_PING_TARGET_ID: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    strncpy(msg_out->target_id, str, sizeof(msg_out->target_id) - 1);
                }
                break;
            }
            
            case FIELD_SEQUENCE_NUMBER:
                tlv_field_get_uint32(&field, &msg_out->sequence_number);
                break;
            
            default:
                break;
        }
    }
    
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

/* ============================================================================
 * GOSSIP MESSAGE SERIALIZATION - Membership Update (with load metrics)
 * ========================================================================= */

distric_err_t serialize_gossip_membership_update(
    const gossip_membership_update_t* msg,
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
    
    tlv_encode_string(enc, FIELD_NODE_ID, msg->sender_id);
    
    /* Encode each node update */
    if (msg->updates && msg->update_count > 0) {
        for (size_t i = 0; i < msg->update_count; i++) {
            const gossip_node_info_t* node = &msg->updates[i];
            
            /* Create sub-encoder for node info */
            tlv_encoder_t* node_enc = tlv_encoder_create(256);
            tlv_encode_string(node_enc, FIELD_NODE_ID, node->node_id);
            tlv_encode_string(node_enc, FIELD_NODE_ADDRESS, node->address);
            tlv_encode_uint16(node_enc, FIELD_NODE_PORT, node->port);
            tlv_encode_uint32(node_enc, FIELD_NODE_STATE, (uint32_t)node->state);
            tlv_encode_uint32(node_enc, FIELD_NODE_ROLE, (uint32_t)node->role);
            tlv_encode_uint64(node_enc, FIELD_INCARNATION, node->incarnation);
            
            /* Add load metrics */
            tlv_encode_uint8(node_enc, FIELD_CPU_USAGE, node->cpu_usage);
            tlv_encode_uint8(node_enc, FIELD_MEMORY_USAGE, node->memory_usage);
            
            size_t node_len;
            uint8_t* node_buf = tlv_encoder_finalize(node_enc, &node_len);
            
            tlv_encode_bytes(enc, FIELD_MEMBERSHIP_UPDATES, node_buf, node_len);
            
            tlv_encoder_free(node_enc);
        }
    }
    
    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_gossip_membership_update(
    const uint8_t* buffer,
    size_t len,
    gossip_membership_update_t* msg_out
) {
    if (!buffer || !msg_out || len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!tlv_validate_buffer(buffer, len)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    memset(msg_out, 0, sizeof(gossip_membership_update_t));
    
    gossip_node_info_t* updates = NULL;
    size_t update_count = 0;
    size_t update_capacity = 0;
    
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_field_t field;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_NODE_ID: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    strncpy(msg_out->sender_id, str, sizeof(msg_out->sender_id) - 1);
                }
                break;
            }
            
            case FIELD_MEMBERSHIP_UPDATES: {
                size_t node_data_len;
                const uint8_t* node_data = tlv_field_get_bytes(&field, &node_data_len);
                
                if (node_data) {
                    /* Grow array if needed */
                    if (update_count >= update_capacity) {
                        update_capacity = update_capacity == 0 ? 4 : update_capacity * 2;
                        gossip_node_info_t* new_updates = (gossip_node_info_t*)realloc(
                            updates, update_capacity * sizeof(gossip_node_info_t));
                        if (!new_updates) {
                            free(updates);
                            tlv_decoder_free(dec);
                            return DISTRIC_ERR_NO_MEMORY;
                        }
                        updates = new_updates;
                    }
                    
                    gossip_node_info_t* node = &updates[update_count];
                    memset(node, 0, sizeof(gossip_node_info_t));
                    
                    /* Decode node fields */
                    tlv_decoder_t* node_dec = tlv_decoder_create(node_data, node_data_len);
                    tlv_field_t node_field;
                    
                    while (tlv_decode_next(node_dec, &node_field) == DISTRIC_OK) {
                        switch (node_field.tag) {
                            case FIELD_NODE_ID: {
                                const char* str = tlv_field_get_string(&node_field);
                                if (str) {
                                    strncpy(node->node_id, str, sizeof(node->node_id) - 1);
                                }
                                break;
                            }
                            
                            case FIELD_NODE_ADDRESS: {
                                const char* str = tlv_field_get_string(&node_field);
                                if (str) {
                                    strncpy(node->address, str, sizeof(node->address) - 1);
                                }
                                break;
                            }
                            
                            case FIELD_NODE_PORT:
                                tlv_field_get_uint16(&node_field, &node->port);
                                break;
                            
                            case FIELD_NODE_STATE: {
                                uint32_t state;
                                tlv_field_get_uint32(&node_field, &state);
                                node->state = (node_state_t)state;
                                break;
                            }
                            
                            case FIELD_NODE_ROLE: {
                                uint32_t role;
                                tlv_field_get_uint32(&node_field, &role);
                                node->role = (node_role_t)role;
                                break;
                            }
                            
                            case FIELD_INCARNATION:
                                tlv_field_get_uint64(&node_field, &node->incarnation);
                                break;
                            
                            case FIELD_CPU_USAGE:
                                tlv_field_get_uint8(&node_field, &node->cpu_usage);
                                break;
                            
                            case FIELD_MEMORY_USAGE:
                                tlv_field_get_uint8(&node_field, &node->memory_usage);
                                break;
                        }
                    }
                    
                    tlv_decoder_free(node_dec);
                    update_count++;
                }
                break;
            }
            
            default:
                break;
        }
    }
    
    msg_out->updates = updates;
    msg_out->update_count = update_count;
    
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

void free_gossip_membership_update(gossip_membership_update_t* msg) {
    if (!msg) {
        return;
    }
    
    free(msg->updates);
    msg->updates = NULL;
    msg->update_count = 0;
}

/* ============================================================================
 * TASK MESSAGE SERIALIZATION - Assignment
 * ========================================================================= */

distric_err_t serialize_task_assignment(
    const task_assignment_t* msg,
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
    
    tlv_encode_string(enc, FIELD_TASK_ID, msg->task_id);
    tlv_encode_string(enc, FIELD_WORKFLOW_ID, msg->workflow_id);
    tlv_encode_string(enc, FIELD_TASK_TYPE, msg->task_type);
    
    if (msg->config_json) {
        tlv_encode_string(enc, FIELD_TASK_CONFIG, msg->config_json);
    }
    
    if (msg->input_data && msg->input_data_len > 0) {
        tlv_encode_bytes(enc, FIELD_TASK_INPUT, msg->input_data, msg->input_data_len);
    }
    
    tlv_encode_uint32(enc, FIELD_TIMEOUT_SEC, msg->timeout_sec);
    tlv_encode_uint32(enc, FIELD_RETRY_COUNT, msg->retry_count);
    
    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_task_assignment(
    const uint8_t* buffer,
    size_t len,
    task_assignment_t* msg_out
) {
    if (!buffer || !msg_out || len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!tlv_validate_buffer(buffer, len)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    memset(msg_out, 0, sizeof(task_assignment_t));
    
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_field_t field;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_TASK_ID: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    strncpy(msg_out->task_id, str, sizeof(msg_out->task_id) - 1);
                }
                break;
            }
            
            case FIELD_WORKFLOW_ID: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    strncpy(msg_out->workflow_id, str, sizeof(msg_out->workflow_id) - 1);
                }
                break;
            }
            
            case FIELD_TASK_TYPE: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    strncpy(msg_out->task_type, str, sizeof(msg_out->task_type) - 1);
                }
                break;
            }
            
            case FIELD_TASK_CONFIG: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    msg_out->config_json = strdup(str);
                }
                break;
            }
            
            case FIELD_TASK_INPUT: {
                size_t data_len;
                const uint8_t* data = tlv_field_get_bytes(&field, &data_len);
                if (data && data_len > 0) {
                    msg_out->input_data = (uint8_t*)malloc(data_len);
                    if (msg_out->input_data) {
                        memcpy(msg_out->input_data, data, data_len);
                        msg_out->input_data_len = data_len;
                    }
                }
                break;
            }
            
            case FIELD_TIMEOUT_SEC:
                tlv_field_get_uint32(&field, &msg_out->timeout_sec);
                break;
            
            case FIELD_RETRY_COUNT:
                tlv_field_get_uint32(&field, &msg_out->retry_count);
                break;
            
            default:
                break;
        }
    }
    
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

void free_task_assignment(task_assignment_t* msg) {
    if (!msg) {
        return;
    }
    
    free(msg->config_json);
    free(msg->input_data);
    
    msg->config_json = NULL;
    msg->input_data = NULL;
    msg->input_data_len = 0;
}

/* ============================================================================
 * TASK MESSAGE SERIALIZATION - Result
 * ========================================================================= */

distric_err_t serialize_task_result(
    const task_result_t* msg,
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
    
    tlv_encode_string(enc, FIELD_TASK_ID, msg->task_id);
    tlv_encode_string(enc, FIELD_WORKER_ID, msg->worker_id);
    tlv_encode_uint32(enc, FIELD_TASK_STATUS, (uint32_t)msg->status);
    
    if (msg->output_data && msg->output_data_len > 0) {
        tlv_encode_bytes(enc, FIELD_TASK_OUTPUT, msg->output_data, msg->output_data_len);
    }
    
    if (msg->error_message) {
        tlv_encode_string(enc, FIELD_TASK_ERROR, msg->error_message);
    }
    
    tlv_encode_int32(enc, FIELD_EXIT_CODE, msg->exit_code);
    tlv_encode_uint64(enc, FIELD_STARTED_AT, msg->started_at);
    tlv_encode_uint64(enc, FIELD_COMPLETED_AT, msg->completed_at);
    
    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_task_result(
    const uint8_t* buffer,
    size_t len,
    task_result_t* msg_out
) {
    if (!buffer || !msg_out || len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!tlv_validate_buffer(buffer, len)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    memset(msg_out, 0, sizeof(task_result_t));
    
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_field_t field;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_TASK_ID: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    strncpy(msg_out->task_id, str, sizeof(msg_out->task_id) - 1);
                }
                break;
            }
            
            case FIELD_WORKER_ID: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    strncpy(msg_out->worker_id, str, sizeof(msg_out->worker_id) - 1);
                }
                break;
            }
            
            case FIELD_TASK_STATUS: {
                uint32_t status;
                tlv_field_get_uint32(&field, &status);
                msg_out->status = (task_status_t)status;
                break;
            }
            
            case FIELD_TASK_OUTPUT: {
                size_t data_len;
                const uint8_t* data = tlv_field_get_bytes(&field, &data_len);
                if (data && data_len > 0) {
                    msg_out->output_data = (uint8_t*)malloc(data_len);
                    if (msg_out->output_data) {
                        memcpy(msg_out->output_data, data, data_len);
                        msg_out->output_data_len = data_len;
                    }
                }
                break;
            }
            
            case FIELD_TASK_ERROR: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    msg_out->error_message = strdup(str);
                }
                break;
            }
            
            case FIELD_EXIT_CODE:
                tlv_field_get_int32(&field, &msg_out->exit_code);
                break;
            
            case FIELD_STARTED_AT:
                tlv_field_get_uint64(&field, &msg_out->started_at);
                break;
            
            case FIELD_COMPLETED_AT:
                tlv_field_get_uint64(&field, &msg_out->completed_at);
                break;
            
            default:
                break;
        }
    }
    
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

void free_task_result(task_result_t* msg) {
    if (!msg) {
        return;
    }
    
    free(msg->output_data);
    free(msg->error_message);
    
    msg->output_data = NULL;
    msg->error_message = NULL;
    msg->output_data_len = 0;
}

/* ============================================================================
 * CLIENT MESSAGE SERIALIZATION - Submit
 * ========================================================================= */

distric_err_t serialize_client_submit(
    const client_submit_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
) {
    if (!msg || !buffer_out || !len_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    tlv_encoder_t* enc = tlv_encoder_create(512);
    if (!enc) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_encode_string(enc, FIELD_MESSAGE_ID, msg->message_id);
    tlv_encode_string(enc, FIELD_EVENT_TYPE, msg->event_type);
    
    if (msg->payload_json) {
        tlv_encode_string(enc, FIELD_MESSAGE_PAYLOAD, msg->payload_json);
    }
    
    tlv_encode_uint64(enc, FIELD_TIMESTAMP, msg->timestamp);
    
    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_client_submit(
    const uint8_t* buffer,
    size_t len,
    client_submit_t* msg_out
) {
    if (!buffer || !msg_out || len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!tlv_validate_buffer(buffer, len)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    memset(msg_out, 0, sizeof(client_submit_t));
    
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_field_t field;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_MESSAGE_ID: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    strncpy(msg_out->message_id, str, sizeof(msg_out->message_id) - 1);
                }
                break;
            }
            
            case FIELD_EVENT_TYPE: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    strncpy(msg_out->event_type, str, sizeof(msg_out->event_type) - 1);
                }
                break;
            }
            
            case FIELD_MESSAGE_PAYLOAD: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    msg_out->payload_json = strdup(str);
                }
                break;
            }
            
            case FIELD_TIMESTAMP:
                tlv_field_get_uint64(&field, &msg_out->timestamp);
                break;
            
            default:
                break;
        }
    }
    
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

void free_client_submit(client_submit_t* msg) {
    if (!msg) {
        return;
    }
    
    free(msg->payload_json);
    msg->payload_json = NULL;
}

/* ============================================================================
 * CLIENT MESSAGE SERIALIZATION - Response
 * ========================================================================= */

distric_err_t serialize_client_response(
    const client_response_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
) {
    if (!msg || !buffer_out || !len_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    tlv_encoder_t* enc = tlv_encoder_create(512);
    if (!enc) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_encode_string(enc, FIELD_MESSAGE_ID, msg->message_id);
    tlv_encode_uint32(enc, FIELD_RESPONSE_CODE, msg->response_code);
    
    if (msg->response_message) {
        tlv_encode_string(enc, FIELD_RESPONSE_MESSAGE, msg->response_message);
    }
    
    /* Encode workflows triggered as array */
    if (msg->workflows_triggered && msg->workflow_count > 0) {
        for (size_t i = 0; i < msg->workflow_count; i++) {
            if (msg->workflows_triggered[i]) {
                tlv_encode_string(enc, FIELD_WORKFLOWS_TRIGGERED, msg->workflows_triggered[i]);
            }
        }
    }
    
    *buffer_out = tlv_encoder_detach(enc, len_out);
    tlv_encoder_free(enc);
    
    return *buffer_out ? DISTRIC_OK : DISTRIC_ERR_NO_MEMORY;
}

distric_err_t deserialize_client_response(
    const uint8_t* buffer,
    size_t len,
    client_response_t* msg_out
) {
    if (!buffer || !msg_out || len == 0) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    if (!tlv_validate_buffer(buffer, len)) {
        return DISTRIC_ERR_INVALID_FORMAT;
    }
    
    memset(msg_out, 0, sizeof(client_response_t));
    
    char** workflows = NULL;
    size_t workflow_count = 0;
    size_t workflow_capacity = 0;
    
    tlv_decoder_t* dec = tlv_decoder_create(buffer, len);
    if (!dec) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    tlv_field_t field;
    while (tlv_decode_next(dec, &field) == DISTRIC_OK) {
        switch (field.tag) {
            case FIELD_MESSAGE_ID: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    strncpy(msg_out->message_id, str, sizeof(msg_out->message_id) - 1);
                }
                break;
            }
            
            case FIELD_RESPONSE_CODE:
                tlv_field_get_uint32(&field, &msg_out->response_code);
                break;
            
            case FIELD_RESPONSE_MESSAGE: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    msg_out->response_message = strdup(str);
                }
                break;
            }
            
            case FIELD_WORKFLOWS_TRIGGERED: {
                const char* str = tlv_field_get_string(&field);
                if (str) {
                    /* Grow array if needed */
                    if (workflow_count >= workflow_capacity) {
                        workflow_capacity = workflow_capacity == 0 ? 4 : workflow_capacity * 2;
                        char** new_workflows = (char**)realloc(workflows, 
                                                               workflow_capacity * sizeof(char*));
                        if (!new_workflows) {
                            for (size_t i = 0; i < workflow_count; i++) {
                                free(workflows[i]);
                            }
                            free(workflows);
                            tlv_decoder_free(dec);
                            return DISTRIC_ERR_NO_MEMORY;
                        }
                        workflows = new_workflows;
                    }
                    
                    workflows[workflow_count++] = strdup(str);
                }
                break;
            }
            
            default:
                break;
        }
    }
    
    msg_out->workflows_triggered = workflows;
    msg_out->workflow_count = workflow_count;
    
    tlv_decoder_free(dec);
    return DISTRIC_OK;
}

void free_client_response(client_response_t* msg) {
    if (!msg) {
        return;
    }
    
    free(msg->response_message);
    
    if (msg->workflows_triggered) {
        for (size_t i = 0; i < msg->workflow_count; i++) {
            free(msg->workflows_triggered[i]);
        }
        free(msg->workflows_triggered);
    }
    
    msg->response_message = NULL;
    msg->workflows_triggered = NULL;
    msg->workflow_count = 0;
}

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

const char* node_state_to_string(node_state_t state) {
    switch (state) {
        case NODE_STATE_ALIVE:      return "ALIVE";
        case NODE_STATE_SUSPECTED:  return "SUSPECTED";
        case NODE_STATE_FAILED:     return "FAILED";
        case NODE_STATE_LEFT:       return "LEFT";
        default:                    return "UNKNOWN";
    }
}

const char* node_role_to_string(node_role_t role) {
    switch (role) {
        case NODE_ROLE_COORDINATOR: return "COORDINATOR";
        case NODE_ROLE_WORKER:      return "WORKER";
        default:                    return "UNKNOWN";
    }
}

const char* task_status_to_string(task_status_t status) {
    switch (status) {
        case TASK_STATUS_PENDING:   return "PENDING";
        case TASK_STATUS_RUNNING:   return "RUNNING";
        case TASK_STATUS_COMPLETED: return "COMPLETED";
        case TASK_STATUS_FAILED:    return "FAILED";
        case TASK_STATUS_TIMEOUT:   return "TIMEOUT";
        case TASK_STATUS_CANCELLED: return "CANCELLED";
        default:                    return "UNKNOWN";
    }
}