/**
 * @file messages.h
 * @brief Protocol Message Definitions - Wire Format Only
 * 
 * IMPORTANT: This file defines WIRE FORMATS for network transmission.
 * It does NOT define internal application data structures.
 * 
 * Defines all message structures used in the DistriC protocol:
 * - Raft consensus messages (wire format)
 * - Gossip protocol messages
 * - Task execution messages
 * - Client API messages
 * 
 * Each message type has:
 * - Field tag constants
 * - C struct definition (WIRE FORMAT)
 * - Serialization function
 * - Deserialization function
 * - Free function (if needed)
 * 
 * All messages are serialized using TLV encoding for flexibility
 * and forward compatibility.
 * 
 * @version 1.0.0
 */

#ifndef DISTRIC_PROTOCOL_MESSAGES_H
#define DISTRIC_PROTOCOL_MESSAGES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <distric_obs.h>
#include "binary.h"
#include "tlv.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * FIELD TAG DEFINITIONS
 * ========================================================================= */

/* Common fields (0x0000-0x00FF) */
#define FIELD_TERM                  0x0001  /**< Raft term number */
#define FIELD_NODE_ID               0x0002  /**< Node identifier */
#define FIELD_TIMESTAMP             0x0003  /**< Unix timestamp */
#define FIELD_SEQUENCE_NUMBER       0x0004  /**< Sequence number */

/* Raft fields (0x0100-0x01FF) */
#define FIELD_CANDIDATE_ID          0x0101  /**< Candidate node ID */
#define FIELD_LAST_LOG_INDEX        0x0102  /**< Last log index */
#define FIELD_LAST_LOG_TERM         0x0103  /**< Last log term */
#define FIELD_VOTE_GRANTED          0x0104  /**< Vote granted (bool) */
#define FIELD_LEADER_ID             0x0105  /**< Leader node ID */
#define FIELD_PREV_LOG_INDEX        0x0106  /**< Previous log index */
#define FIELD_PREV_LOG_TERM         0x0107  /**< Previous log term */
#define FIELD_ENTRIES               0x0108  /**< Log entries (array) */
#define FIELD_LEADER_COMMIT         0x0109  /**< Leader commit index */
#define FIELD_SUCCESS               0x010A  /**< Operation success (bool) */
#define FIELD_SNAPSHOT_INDEX        0x010B  /**< Snapshot last index */
#define FIELD_SNAPSHOT_TERM         0x010C  /**< Snapshot last term */
#define FIELD_SNAPSHOT_OFFSET       0x010D  /**< Snapshot chunk offset */
#define FIELD_SNAPSHOT_DATA         0x010E  /**< Snapshot chunk data */
#define FIELD_SNAPSHOT_DONE         0x010F  /**< Snapshot transfer done */

/* Log entry fields (0x0110-0x011F) */
#define FIELD_ENTRY_INDEX           0x0110  /**< Entry index */
#define FIELD_ENTRY_TERM            0x0111  /**< Entry term */
#define FIELD_ENTRY_DATA            0x0112  /**< Entry data */
#define FIELD_ENTRY_TYPE            0x0113  /**< Entry type */

/* Configuration change (0x0120-0x012F) */
#define FIELD_CONFIG_CHANGE_TYPE    0x0120  /**< Config change type */
#define FIELD_NODE_TO_ADD           0x0121  /**< Node being added */
#define FIELD_NODE_TO_REMOVE        0x0122  /**< Node being removed */
#define FIELD_NEW_SERVERS           0x0123  /**< New server list (C_new) */
#define FIELD_OLD_SERVERS           0x0124  /**< Old server list (C_old) */

/* Gossip fields (0x0200-0x02FF) */
#define FIELD_INCARNATION           0x0201  /**< Incarnation number */
#define FIELD_NODE_STATE            0x0202  /**< Node state */
#define FIELD_NODE_ADDRESS          0x0203  /**< Node address */
#define FIELD_NODE_PORT             0x0204  /**< Node port */
#define FIELD_NODE_ROLE             0x0205  /**< Node role (coordinator/worker) */
#define FIELD_SUSPECT_NODE_ID       0x0206  /**< Suspected node ID */
#define FIELD_PING_TARGET_ID        0x0207  /**< Ping target node ID */
#define FIELD_INDIRECT_TARGET_ID    0x0208  /**< Indirect ping target */
#define FIELD_MEMBERSHIP_UPDATES    0x0209  /**< Membership update array */
#define FIELD_PROTOCOL_VERSION      0x020A  /**< Protocol version */
#define FIELD_METADATA              0x020B  /**< Node metadata */

/* Load metrics (0x0220-0x022F) */
#define FIELD_CPU_USAGE             0x0220  /**< CPU usage percentage (0-100) */
#define FIELD_MEMORY_USAGE          0x0221  /**< Memory usage percentage (0-100) */

/* Task fields (0x0300-0x03FF) */
#define FIELD_TASK_ID               0x0301  /**< Task identifier */
#define FIELD_WORKFLOW_ID           0x0302  /**< Workflow identifier */
#define FIELD_TASK_TYPE             0x0303  /**< Task type/plugin name */
#define FIELD_TASK_CONFIG           0x0304  /**< Task configuration (JSON) */
#define FIELD_TASK_INPUT            0x0305  /**< Task input data */
#define FIELD_TASK_OUTPUT           0x0306  /**< Task output data */
#define FIELD_TASK_STATUS           0x0307  /**< Task status */
#define FIELD_TASK_ERROR            0x0308  /**< Task error message */
#define FIELD_TIMEOUT_SEC           0x0309  /**< Timeout in seconds */
#define FIELD_RETRY_COUNT           0x030A  /**< Retry count */
#define FIELD_WORKER_ID             0x030B  /**< Assigned worker ID */
#define FIELD_STARTED_AT            0x030C  /**< Start timestamp */
#define FIELD_COMPLETED_AT          0x030D  /**< Completion timestamp */
#define FIELD_EXIT_CODE             0x030E  /**< Task exit code */

/* Client fields (0x0400-0x04FF) */
#define FIELD_MESSAGE_ID            0x0401  /**< Message identifier */
#define FIELD_EVENT_TYPE            0x0402  /**< Event type */
#define FIELD_MESSAGE_PAYLOAD       0x0403  /**< Message payload (JSON) */
#define FIELD_RESPONSE_CODE         0x0404  /**< Response code */
#define FIELD_RESPONSE_MESSAGE      0x0405  /**< Response message */
#define FIELD_WORKFLOWS_TRIGGERED   0x0406  /**< Triggered workflow IDs */
#define FIELD_ERROR_CODE            0x0407  /**< Error code */
#define FIELD_ERROR_DETAILS         0x0408  /**< Error details */
#define FIELD_QUERY_TYPE            0x0409  /**< Query type */
#define FIELD_QUERY_PARAMS          0x040A  /**< Query parameters */

/* ============================================================================
 * WIRE FORMAT ENUMS - Protocol Level Only
 * ========================================================================= */

/**
 * @brief Node state in gossip protocol (wire format)
 */
typedef enum {
    NODE_STATE_ALIVE = 0,
    NODE_STATE_SUSPECTED = 1,
    NODE_STATE_FAILED = 2,
    NODE_STATE_LEFT = 3
} node_state_t;

/**
 * @brief Node role (wire format)
 */
typedef enum {
    NODE_ROLE_COORDINATOR = 0,
    NODE_ROLE_WORKER = 1
} node_role_t;

/**
 * @brief Task status (wire format)
 */
typedef enum {
    TASK_STATUS_PENDING = 0,
    TASK_STATUS_RUNNING = 1,
    TASK_STATUS_COMPLETED = 2,
    TASK_STATUS_FAILED = 3,
    TASK_STATUS_TIMEOUT = 4,
    TASK_STATUS_CANCELLED = 5
} task_status_t;

/**
 * @brief Configuration change type (wire format)
 */
typedef enum {
    CONFIG_CHANGE_ADD_NODE = 0,
    CONFIG_CHANGE_REMOVE_NODE = 1,
    CONFIG_CHANGE_REPLACE = 2
} config_change_type_t;

/**
 * @brief Raft entry type (wire format)
 */
typedef enum {
    RAFT_ENTRY_NORMAL = 0,
    RAFT_ENTRY_CONFIG = 1,
    RAFT_ENTRY_NOOP = 2
} raft_entry_type_t;

/* ============================================================================
 * RAFT WIRE FORMAT MESSAGES
 * 
 * These are PURE WIRE FORMATS for network transmission.
 * They use uint8_t for entry_type to ensure unambiguous serialization.
 * Applications should convert to/from their internal types.
 * ========================================================================= */

/**
 * @brief Raft log entry - WIRE FORMAT ONLY
 * 
 * This is the protocol-level representation.
 * Applications (like distric_raft) maintain their own internal types.
 */
typedef struct {
    uint32_t term;              /**< Entry term */
    uint32_t index;             /**< Entry index */
    uint8_t entry_type;         /**< Entry type: 0=normal, 1=config, 2=noop */
    uint8_t* data;              /**< Entry data */
    size_t data_len;            /**< Data length */
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
    raft_log_entry_wire_t* entries;  /**< WIRE FORMAT entries */
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
    uint32_t role;  /**< Wire format: uint32_t */
} raft_server_info_t;

/**
 * @brief Raft configuration change - WIRE FORMAT
 */
typedef struct {
    uint32_t type;  /**< Wire format: uint32_t */
    
    /* Single node change */
    raft_server_info_t node_info;
    
    /* Joint consensus */
    raft_server_info_t* old_servers;
    size_t old_server_count;
    raft_server_info_t* new_servers;
    size_t new_server_count;
} raft_configuration_change_t;

/* ============================================================================
 * GOSSIP WIRE FORMAT MESSAGES
 * ========================================================================= */

/**
 * @brief Gossip node info - WIRE FORMAT
 */
typedef struct {
    char node_id[64];
    char address[256];
    uint16_t port;
    uint32_t state;        /**< Wire: uint32_t */
    uint32_t role;         /**< Wire: uint32_t */
    uint64_t incarnation;
    uint8_t cpu_usage;
    uint8_t memory_usage;
} gossip_node_info_t;

/**
 * @brief Gossip Ping - WIRE FORMAT
 */
typedef struct {
    char sender_id[64];
    uint64_t incarnation;
    uint32_t sequence_number;
} gossip_ping_t;

/**
 * @brief Gossip Ack - WIRE FORMAT
 */
typedef struct {
    char sender_id[64];
    uint64_t incarnation;
    uint32_t sequence_number;
} gossip_ack_t;

/**
 * @brief Gossip Indirect Ping - WIRE FORMAT
 */
typedef struct {
    char sender_id[64];
    char target_id[64];
    uint32_t sequence_number;
} gossip_indirect_ping_t;

/**
 * @brief Gossip Membership Update - WIRE FORMAT
 */
typedef struct {
    char sender_id[64];
    gossip_node_info_t* updates;
    size_t update_count;
} gossip_membership_update_t;

/* ============================================================================
 * TASK WIRE FORMAT MESSAGES
 * ========================================================================= */

/**
 * @brief Task Assignment - WIRE FORMAT
 */
typedef struct {
    char task_id[128];
    char workflow_id[128];
    char task_type[64];
    char* config_json;
    uint8_t* input_data;
    size_t input_data_len;
    uint32_t timeout_sec;
    uint32_t retry_count;
} task_assignment_t;

/**
 * @brief Task Result - WIRE FORMAT
 */
typedef struct {
    char task_id[128];
    char worker_id[64];
    uint32_t status;  /**< Wire: uint32_t */
    uint8_t* output_data;
    size_t output_data_len;
    char* error_message;
    int32_t exit_code;
    uint64_t started_at;
    uint64_t completed_at;
} task_result_t;

/* ============================================================================
 * CLIENT WIRE FORMAT MESSAGES
 * ========================================================================= */

/**
 * @brief Client Submit - WIRE FORMAT
 */
typedef struct {
    char message_id[128];
    char event_type[64];
    char* payload_json;
    uint64_t timestamp;
} client_submit_t;

/**
 * @brief Client Response - WIRE FORMAT
 */
typedef struct {
    char message_id[128];
    uint32_t response_code;
    char* response_message;
    char** workflows_triggered;
    size_t workflow_count;
} client_response_t;

/* ============================================================================
 * RAFT SERIALIZATION FUNCTIONS
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
 * GOSSIP SERIALIZATION FUNCTIONS
 * ========================================================================= */

distric_err_t serialize_gossip_ping(
    const gossip_ping_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
);

distric_err_t deserialize_gossip_ping(
    const uint8_t* buffer,
    size_t len,
    gossip_ping_t* msg_out
);

distric_err_t serialize_gossip_ack(
    const gossip_ack_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
);

distric_err_t deserialize_gossip_ack(
    const uint8_t* buffer,
    size_t len,
    gossip_ack_t* msg_out
);

distric_err_t serialize_gossip_indirect_ping(
    const gossip_indirect_ping_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
);

distric_err_t deserialize_gossip_indirect_ping(
    const uint8_t* buffer,
    size_t len,
    gossip_indirect_ping_t* msg_out
);

distric_err_t serialize_gossip_membership_update(
    const gossip_membership_update_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
);

distric_err_t deserialize_gossip_membership_update(
    const uint8_t* buffer,
    size_t len,
    gossip_membership_update_t* msg_out
);

void free_gossip_membership_update(gossip_membership_update_t* msg);

/* ============================================================================
 * TASK SERIALIZATION FUNCTIONS
 * ========================================================================= */

distric_err_t serialize_task_assignment(
    const task_assignment_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
);

distric_err_t deserialize_task_assignment(
    const uint8_t* buffer,
    size_t len,
    task_assignment_t* msg_out
);

void free_task_assignment(task_assignment_t* msg);

distric_err_t serialize_task_result(
    const task_result_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
);

distric_err_t deserialize_task_result(
    const uint8_t* buffer,
    size_t len,
    task_result_t* msg_out
);

void free_task_result(task_result_t* msg);

/* ============================================================================
 * CLIENT SERIALIZATION FUNCTIONS
 * ========================================================================= */

distric_err_t serialize_client_submit(
    const client_submit_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
);

distric_err_t deserialize_client_submit(
    const uint8_t* buffer,
    size_t len,
    client_submit_t* msg_out
);

void free_client_submit(client_submit_t* msg);

distric_err_t serialize_client_response(
    const client_response_t* msg,
    uint8_t** buffer_out,
    size_t* len_out
);

distric_err_t deserialize_client_response(
    const uint8_t* buffer,
    size_t len,
    client_response_t* msg_out
);

void free_client_response(client_response_t* msg);

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

const char* node_state_to_string(node_state_t state);
const char* node_role_to_string(node_role_t role);
const char* task_status_to_string(task_status_t status);
const char* raft_entry_type_to_string(raft_entry_type_t type);
const char* config_change_type_to_string(config_change_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_PROTOCOL_MESSAGES_H */