/**
 * @file raft_messages.h
 * @brief Raft Message Serialization
 */

#ifndef DISTRIC_RAFT_MESSAGES_H
#define DISTRIC_RAFT_MESSAGES_H

#include <distric_obs.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * RAFT MESSAGE STRUCTURES
 * ========================================================================= */

typedef struct {
    uint32_t term;
    char candidate_id[64];
    uint32_t last_log_index;
    uint32_t last_log_term;
} raft_request_vote_t;

typedef struct {
    uint32_t term;
    bool vote_granted;
} raft_request_vote_response_t;

typedef struct {
    uint32_t term;
    uint32_t index;
    uint8_t entry_type;
    uint8_t* data;
    uint32_t data_len;
} raft_log_entry_wire_t;

typedef struct {
    uint32_t term;
    char leader_id[64];
    uint32_t prev_log_index;
    uint32_t prev_log_term;
    uint32_t leader_commit;
    raft_log_entry_wire_t* entries;
    size_t entry_count;
} raft_append_entries_t;

typedef struct {
    uint32_t term;
    bool success;
    uint32_t match_index;
} raft_append_entries_response_t;

typedef struct {
    uint32_t term;
    char leader_id[64];
    uint32_t last_included_index;
    uint32_t last_included_term;
    uint8_t* data;
    size_t data_len;
} raft_install_snapshot_t;

typedef struct {
    uint32_t term;
    bool success;
} raft_install_snapshot_response_t;

/* ============================================================================
 * SERIALIZATION FUNCTIONS
 * ========================================================================= */

distric_err_t serialize_raft_request_vote(
    const raft_request_vote_t* req,
    uint8_t** buf_out,
    size_t* len_out
);

distric_err_t deserialize_raft_request_vote(
    const uint8_t* buf,
    size_t len,
    raft_request_vote_t* req_out
);

void free_raft_request_vote(raft_request_vote_t* req);

distric_err_t serialize_raft_request_vote_response(
    const raft_request_vote_response_t* resp,
    uint8_t** buf_out,
    size_t* len_out
);

distric_err_t deserialize_raft_request_vote_response(
    const uint8_t* buf,
    size_t len,
    raft_request_vote_response_t* resp_out
);

distric_err_t serialize_raft_append_entries(
    const raft_append_entries_t* req,
    uint8_t** buf_out,
    size_t* len_out
);

distric_err_t deserialize_raft_append_entries(
    const uint8_t* buf,
    size_t len,
    raft_append_entries_t* req_out
);

void free_raft_append_entries(raft_append_entries_t* req);

distric_err_t serialize_raft_append_entries_response(
    const raft_append_entries_response_t* resp,
    uint8_t** buf_out,
    size_t* len_out
);

distric_err_t deserialize_raft_append_entries_response(
    const uint8_t* buf,
    size_t len,
    raft_append_entries_response_t* resp_out
);

distric_err_t serialize_raft_install_snapshot(
    const raft_install_snapshot_t* req,
    uint8_t** buf_out,
    size_t* len_out
);

distric_err_t deserialize_raft_install_snapshot(
    const uint8_t* buf,
    size_t len,
    raft_install_snapshot_t* req_out
);

void free_raft_install_snapshot(raft_install_snapshot_t* req);

distric_err_t serialize_raft_install_snapshot_response(
    const raft_install_snapshot_response_t* resp,
    uint8_t** buf_out,
    size_t* len_out
);

distric_err_t deserialize_raft_install_snapshot_response(
    const uint8_t* buf,
    size_t len,
    raft_install_snapshot_response_t* resp_out
);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_RAFT_MESSAGES_H */