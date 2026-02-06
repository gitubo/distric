/**
 * @file raft_messages.h
 * @brief Raft Message Definitions and Serialization
 * 
 * Defines Raft-specific message structures and serialization functions.
 * Uses distric_protocol's TLV encoding primitives.
 * 
 * @version 1.0.0
 */

#ifndef DISTRIC_RAFT_MESSAGES_H
#define DISTRIC_RAFT_MESSAGES_H

#include <distric_obs.h>
#include <distric_protocol.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * RAFT FIELD TAG DEFINITIONS (0x0100-0x01FF range)
 * ========================================================================= */

#define FIELD_TERM                  0x0101  /**< Raft term number */
#define FIELD_CANDIDATE_ID          0x0102  /**< Candidate node ID */
#define FIELD_LAST_LOG_INDEX        0x0103  /**< Last log index */
#define FIELD_LAST_LOG_TERM         0x0104  /**< Last log term */
#define FIELD_VOTE_GRANTED          0x0105  /**< Vote granted (bool) */
#define FIELD_LEADER_ID             0x0106  /**< Leader node ID */
#define FIELD_PREV_LOG_INDEX        0x0107  /**< Previous log index */
#define FIELD_PREV_LOG_TERM         0x0108  /**< Previous log term */
#define FIELD_ENTRIES               0x0109  /**< Log entries (array) */
#define FIELD_LEADER_COMMIT         0x010A  /**< Leader commit index */
#define FIELD_SUCCESS               0x010B  /**< Operation success (bool) */
#define FIELD_SNAPSHOT_INDEX        0x010C  /**< Snapshot last index */
#define FIELD_SNAPSHOT_TERM         0x010D  /**< Snapshot last term */
#define FIELD_SNAPSHOT_DATA         0x010E  /**< Snapshot data */

/* Log entry fields */
#define FIELD_ENTRY_INDEX           0x0110  /**< Entry index */
#define FIELD_ENTRY_TERM            0x0111  /**< Entry term */
#define FIELD_ENTRY_TYPE            0x0112  /**< Entry type */
#define FIELD_ENTRY_DATA            0x0113  /**< Entry data */

/* Configuration change fields */
#define FIELD_CONFIG_CHANGE_TYPE    0x0120  /**< Config change type */
#define FIELD_NODE_TO_ADD           0x0121  /**< Node being added */
#define FIELD_NODE_TO_REMOVE        0x0122  /**< Node being removed */
#define FIELD_NEW_SERVERS           0x0123  /**< New server list */
#define FIELD_OLD_SERVERS           0x0124  /**< Old server list */

/* ============================================================================
 * RAFT WIRE FORMAT ENUMS
 * ========================================================================= */

/**
 * @brief Raft entry type (wire format)
 */
#define RAFT_ENTRY_NORMAL 0
#define RAFT_ENTRY_CONFIG 1
#define RAFT_ENTRY_NOOP   2

/**
 * @brief Configuration change type (wire format)
 */
typedef enum {
    CONFIG_CHANGE_ADD_NODE = 0,
    CONFIG_CHANGE_REMOVE_NODE = 1,
    CONFIG_CHANGE_REPLACE = 2
} config_change_type_t;

/* ============================================================================
 * RAFT WIRE FORMAT MESSAGES
 * ========================================================================= */

/**
 * @brief Raft log entry - WIRE FORMAT
 */
typedef struct {
    uint32_t term;
    uint32_t index;
    uint8_t entry_type;  /**< 0=normal, 1=config, 2=noop */
    uint8_t* data;
    size_t data_len;
} raft_log_entry_wire_t;

/**
 * @brief Raft RequestVote RPC - WIRE FORMAT
 */
typedef struct {
    uint32_t term;
    char candidate_id[64];
    uint32_t last_log_index;
    uint32_t last_log_term;
} raft_request_vote_t;

/**
 * @brief Raft RequestVote Response - WIRE FORMAT
 */
typedef struct {
    uint32_t term;
    bool vote_granted;
    char node_id[64];
} raft_request_vote_response_t;

/**
 * @brief Raft AppendEntries RPC - WIRE FORMAT
 */
typedef struct {
    uint32_t term;
    char leader_id[64];
    uint32_t prev_log_index;
    uint32_t prev_log_term;
    raft_log_entry_wire_t* entries;
    size_t entry_count;
    uint32_t leader_commit;
} raft_append_entries_t;

/**
 * @brief Raft AppendEntries Response - WIRE FORMAT
 */
typedef struct {
    uint32_t term;
    bool success;
    char node_id[64];
    uint32_t match_index;
} raft_append_entries_response_t;

/**
 * @brief Raft InstallSnapshot RPC - WIRE FORMAT
 */
typedef struct {
    uint32_t term;
    char leader_id[64];
    uint32_t last_included_index;
    uint32_t last_included_term;
    uint8_t* data;
    size_t data_len;
} raft_install_snapshot_t;

/**
 * @brief Raft InstallSnapshot Response - WIRE FORMAT
 */
typedef struct {
    uint32_t term;
    char node_id[64];
    bool success;
} raft_install_snapshot_response_t;

/**
 * @brief Server info for configuration - WIRE FORMAT
 */
typedef struct {
    char node_id[64];
    char address[256];
    uint16_t port;
    uint32_t role;
} raft_server_info_t;

/**
 * @brief Raft configuration change - WIRE FORMAT
 */
typedef struct {
    uint32_t type;
    raft_server_info_t node_info;
    raft_server_info_t* old_servers;
    size_t old_server_count;
    raft_server_info_t* new_servers;
    size_t new_server_count;
} raft_configuration_change_t;

/* ============================================================================
 * SERIALIZATION FUNCTIONS
 * ========================================================================= */

distric_err_t serialize_raft_request_vote(
    const raft_request_vote_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
);

distric_err_t deserialize_raft_request_vote(
    const uint8_t* buffer,
    size_t len,
    raft_request_vote_t* msg_out
);

void free_raft_request_vote(raft_request_vote_t* msg);

distric_err_t serialize_raft_request_vote_response(
    const raft_request_vote_response_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
);

distric_err_t deserialize_raft_request_vote_response(
    const uint8_t* buffer,
    size_t len,
    raft_request_vote_response_t* msg_out
);

distric_err_t serialize_raft_append_entries(
    const raft_append_entries_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
);

distric_err_t deserialize_raft_append_entries(
    const uint8_t* buffer,
    size_t len,
    raft_append_entries_t* msg_out
);

void free_raft_append_entries(raft_append_entries_t* msg);

distric_err_t serialize_raft_append_entries_response(
    const raft_append_entries_response_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
);

distric_err_t deserialize_raft_append_entries_response(
    const uint8_t* buffer,
    size_t len,
    raft_append_entries_response_t* msg_out
);

distric_err_t serialize_raft_install_snapshot(
    const raft_install_snapshot_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
);

distric_err_t deserialize_raft_install_snapshot(
    const uint8_t* buffer,
    size_t len,
    raft_install_snapshot_t* msg_out
);

void free_raft_install_snapshot(raft_install_snapshot_t* msg);

distric_err_t serialize_raft_install_snapshot_response(
    const raft_install_snapshot_response_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
);

distric_err_t deserialize_raft_install_snapshot_response(
    const uint8_t* buffer,
    size_t len,
    raft_install_snapshot_response_t* msg_out
);

distric_err_t serialize_raft_configuration_change(
    const raft_configuration_change_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
);

distric_err_t deserialize_raft_configuration_change(
    const uint8_t* buffer,
    size_t len,
    raft_configuration_change_t* msg_out
);

void free_raft_configuration_change(raft_configuration_change_t* msg);

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

const char* raft_entry_type_to_string(uint8_t type);
const char* config_change_type_to_string(config_change_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_RAFT_MESSAGES_H */