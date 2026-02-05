/**
 * @file raft_messages.c
 * @brief Raft Message Serialization Implementation
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
#include <arpa/inet.h>

/* ============================================================================
 * SERIALIZATION HELPERS
 * ========================================================================= */

static void write_uint32(uint8_t** ptr, uint32_t value) {
    uint32_t net_value = htonl(value);
    memcpy(*ptr, &net_value, sizeof(uint32_t));
    *ptr += sizeof(uint32_t);
}

static uint32_t read_uint32(const uint8_t** ptr) {
    uint32_t net_value;
    memcpy(&net_value, *ptr, sizeof(uint32_t));
    *ptr += sizeof(uint32_t);
    return ntohl(net_value);
}

static void write_string(uint8_t** ptr, const char* str, size_t max_len) {
    size_t len = str ? strlen(str) : 0;
    if (len > max_len - 1) len = max_len - 1;
    
    write_uint32(ptr, (uint32_t)len);
    if (len > 0) {
        memcpy(*ptr, str, len);
        *ptr += len;
    }
}

static void read_string(const uint8_t** ptr, char* str, size_t max_len) {
    uint32_t len = read_uint32(ptr);
    if (len > max_len - 1) len = max_len - 1;
    
    if (len > 0) {
        memcpy(str, *ptr, len);
        *ptr += len;
    }
    str[len] = '\0';
}

/* ============================================================================
 * REQUEST VOTE
 * ========================================================================= */

distric_err_t serialize_raft_request_vote(
    const raft_request_vote_t* req,
    uint8_t** buf_out,
    size_t* len_out
) {
    if (!req || !buf_out || !len_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    size_t size = sizeof(uint32_t) * 3 + 64;
    uint8_t* buf = (uint8_t*)malloc(size);
    if (!buf) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    uint8_t* ptr = buf;
    write_uint32(&ptr, req->term);
    write_string(&ptr, req->candidate_id, 64);
    write_uint32(&ptr, req->last_log_index);
    write_uint32(&ptr, req->last_log_term);
    
    *buf_out = buf;
    *len_out = ptr - buf;
    
    return DISTRIC_OK;
}

distric_err_t deserialize_raft_request_vote(
    const uint8_t* buf,
    size_t len,
    raft_request_vote_t* req_out
) {
    if (!buf || !req_out || len < 16) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    const uint8_t* ptr = buf;
    req_out->term = read_uint32(&ptr);
    read_string(&ptr, req_out->candidate_id, 64);
    req_out->last_log_index = read_uint32(&ptr);
    req_out->last_log_term = read_uint32(&ptr);
    
    return DISTRIC_OK;
}

void free_raft_request_vote(raft_request_vote_t* req) {
    (void)req;
}

distric_err_t serialize_raft_request_vote_response(
    const raft_request_vote_response_t* resp,
    uint8_t** buf_out,
    size_t* len_out
) {
    if (!resp || !buf_out || !len_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    size_t size = sizeof(uint32_t) + sizeof(uint8_t);
    uint8_t* buf = (uint8_t*)malloc(size);
    if (!buf) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    uint8_t* ptr = buf;
    write_uint32(&ptr, resp->term);
    *ptr++ = resp->vote_granted ? 1 : 0;
    
    *buf_out = buf;
    *len_out = ptr - buf;
    
    return DISTRIC_OK;
}

distric_err_t deserialize_raft_request_vote_response(
    const uint8_t* buf,
    size_t len,
    raft_request_vote_response_t* resp_out
) {
    if (!buf || !resp_out || len < 5) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    const uint8_t* ptr = buf;
    resp_out->term = read_uint32(&ptr);
    resp_out->vote_granted = (*ptr++ != 0);
    
    return DISTRIC_OK;
}

/* ============================================================================
 * APPEND ENTRIES
 * ========================================================================= */

distric_err_t serialize_raft_append_entries(
    const raft_append_entries_t* req,
    uint8_t** buf_out,
    size_t* len_out
) {
    if (!req || !buf_out || !len_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    size_t size = sizeof(uint32_t) * 5 + 64 + sizeof(uint32_t);
    for (size_t i = 0; i < req->entry_count; i++) {
        size += sizeof(uint32_t) * 3 + sizeof(uint8_t) + req->entries[i].data_len;
    }
    
    uint8_t* buf = (uint8_t*)malloc(size);
    if (!buf) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    uint8_t* ptr = buf;
    write_uint32(&ptr, req->term);
    write_string(&ptr, req->leader_id, 64);
    write_uint32(&ptr, req->prev_log_index);
    write_uint32(&ptr, req->prev_log_term);
    write_uint32(&ptr, req->leader_commit);
    write_uint32(&ptr, (uint32_t)req->entry_count);
    
    for (size_t i = 0; i < req->entry_count; i++) {
        write_uint32(&ptr, req->entries[i].term);
        write_uint32(&ptr, req->entries[i].index);
        *ptr++ = req->entries[i].entry_type;
        write_uint32(&ptr, req->entries[i].data_len);
        if (req->entries[i].data_len > 0) {
            memcpy(ptr, req->entries[i].data, req->entries[i].data_len);
            ptr += req->entries[i].data_len;
        }
    }
    
    *buf_out = buf;
    *len_out = ptr - buf;
    
    return DISTRIC_OK;
}

distric_err_t deserialize_raft_append_entries(
    const uint8_t* buf,
    size_t len,
    raft_append_entries_t* req_out
) {
    if (!buf || !req_out || len < 24) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    const uint8_t* ptr = buf;
    req_out->term = read_uint32(&ptr);
    read_string(&ptr, req_out->leader_id, 64);
    req_out->prev_log_index = read_uint32(&ptr);
    req_out->prev_log_term = read_uint32(&ptr);
    req_out->leader_commit = read_uint32(&ptr);
    req_out->entry_count = read_uint32(&ptr);
    
    if (req_out->entry_count > 0) {
        req_out->entries = (raft_log_entry_wire_t*)calloc(
            req_out->entry_count, sizeof(raft_log_entry_wire_t));
        if (!req_out->entries) {
            return DISTRIC_ERR_NO_MEMORY;
        }
        
        for (size_t i = 0; i < req_out->entry_count; i++) {
            req_out->entries[i].term = read_uint32(&ptr);
            req_out->entries[i].index = read_uint32(&ptr);
            req_out->entries[i].entry_type = *ptr++;
            req_out->entries[i].data_len = read_uint32(&ptr);
            
            if (req_out->entries[i].data_len > 0) {
                req_out->entries[i].data = (uint8_t*)malloc(req_out->entries[i].data_len);
                if (!req_out->entries[i].data) {
                    for (size_t j = 0; j < i; j++) {
                        free(req_out->entries[j].data);
                    }
                    free(req_out->entries);
                    return DISTRIC_ERR_NO_MEMORY;
                }
                memcpy(req_out->entries[i].data, ptr, req_out->entries[i].data_len);
                ptr += req_out->entries[i].data_len;
            }
        }
    } else {
        req_out->entries = NULL;
    }
    
    return DISTRIC_OK;
}

void free_raft_append_entries(raft_append_entries_t* req) {
    if (!req) return;
    
    if (req->entries) {
        for (size_t i = 0; i < req->entry_count; i++) {
            free(req->entries[i].data);
        }
        free(req->entries);
    }
}

distric_err_t serialize_raft_append_entries_response(
    const raft_append_entries_response_t* resp,
    uint8_t** buf_out,
    size_t* len_out
) {
    if (!resp || !buf_out || !len_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    size_t size = sizeof(uint32_t) * 2 + sizeof(uint8_t);
    uint8_t* buf = (uint8_t*)malloc(size);
    if (!buf) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    uint8_t* ptr = buf;
    write_uint32(&ptr, resp->term);
    *ptr++ = resp->success ? 1 : 0;
    write_uint32(&ptr, resp->match_index);
    
    *buf_out = buf;
    *len_out = ptr - buf;
    
    return DISTRIC_OK;
}

distric_err_t deserialize_raft_append_entries_response(
    const uint8_t* buf,
    size_t len,
    raft_append_entries_response_t* resp_out
) {
    if (!buf || !resp_out || len < 9) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    const uint8_t* ptr = buf;
    resp_out->term = read_uint32(&ptr);
    resp_out->success = (*ptr++ != 0);
    resp_out->match_index = read_uint32(&ptr);
    
    return DISTRIC_OK;
}

/* ============================================================================
 * INSTALL SNAPSHOT
 * ========================================================================= */

distric_err_t serialize_raft_install_snapshot(
    const raft_install_snapshot_t* req,
    uint8_t** buf_out,
    size_t* len_out
) {
    if (!req || !buf_out || !len_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    size_t size = sizeof(uint32_t) * 4 + 64 + req->data_len;
    uint8_t* buf = (uint8_t*)malloc(size);
    if (!buf) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    uint8_t* ptr = buf;
    write_uint32(&ptr, req->term);
    write_string(&ptr, req->leader_id, 64);
    write_uint32(&ptr, req->last_included_index);
    write_uint32(&ptr, req->last_included_term);
    write_uint32(&ptr, (uint32_t)req->data_len);
    
    if (req->data_len > 0 && req->data) {
        memcpy(ptr, req->data, req->data_len);
        ptr += req->data_len;
    }
    
    *buf_out = buf;
    *len_out = ptr - buf;
    
    return DISTRIC_OK;
}

distric_err_t deserialize_raft_install_snapshot(
    const uint8_t* buf,
    size_t len,
    raft_install_snapshot_t* req_out
) {
    if (!buf || !req_out || len < 20) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    const uint8_t* ptr = buf;
    req_out->term = read_uint32(&ptr);
    read_string(&ptr, req_out->leader_id, 64);
    req_out->last_included_index = read_uint32(&ptr);
    req_out->last_included_term = read_uint32(&ptr);
    req_out->data_len = read_uint32(&ptr);
    
    if (req_out->data_len > 0) {
        req_out->data = (uint8_t*)malloc(req_out->data_len);
        if (!req_out->data) {
            return DISTRIC_ERR_NO_MEMORY;
        }
        memcpy(req_out->data, ptr, req_out->data_len);
    } else {
        req_out->data = NULL;
    }
    
    return DISTRIC_OK;
}

void free_raft_install_snapshot(raft_install_snapshot_t* req) {
    if (!req) return;
    free(req->data);
}

distric_err_t serialize_raft_install_snapshot_response(
    const raft_install_snapshot_response_t* resp,
    uint8_t** buf_out,
    size_t* len_out
) {
    if (!resp || !buf_out || !len_out) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    size_t size = sizeof(uint32_t) + sizeof(uint8_t);
    uint8_t* buf = (uint8_t*)malloc(size);
    if (!buf) {
        return DISTRIC_ERR_NO_MEMORY;
    }
    
    uint8_t* ptr = buf;
    write_uint32(&ptr, resp->term);
    *ptr++ = resp->success ? 1 : 0;
    
    *buf_out = buf;
    *len_out = ptr - buf;
    
    return DISTRIC_OK;
}

distric_err_t deserialize_raft_install_snapshot_response(
    const uint8_t* buf,
    size_t len,
    raft_install_snapshot_response_t* resp_out
) {
    if (!buf || !resp_out || len < 5) {
        return DISTRIC_ERR_INVALID_ARG;
    }
    
    const uint8_t* ptr = buf;
    resp_out->term = read_uint32(&ptr);
    resp_out->success = (*ptr++ != 0);
    
    return DISTRIC_OK;
}