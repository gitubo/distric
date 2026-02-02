/**
 * @file messages.h
 * @brief Protocol Message Definitions
 * 
 * Defines all message structures used in the DistriC protocol:
 * - Raft consensus messages
 * - Gossip protocol messages
 * - Task execution messages
 * - Client API messages
 * 
 * Each message type has:
 * - Field tag constants
 * - C struct definition
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
 * RAFT ENTRY TYPES
 * ========================================================================= */

/**
 * @brief Raft log entry type
 */
typedef enum {
    RAFT_ENTRY_NORMAL = 0,      /**< Normal application command */
    RAFT_ENTRY_CONFIG = 1,      /**< Configuration change */
    RAFT_ENTRY_NOOP = 2         /**< No-op (for leader election) */
} raft_entry_type_t;

/* ============================================================================
 * RAFT CONFIGURATION CHANGE
 * ========================================================================= */

/**
 * @brief Configuration change type
 */
typedef enum {
    CONFIG_CHANGE_ADD_NODE = 0,     /**< Add single node */
    CONFIG_CHANGE_REMOVE_NODE = 1,  /**< Remove single node */
    CONFIG_CHANGE_REPLACE = 2       /**< Joint consensus (C_old,new) */
} config_change_type_t;

/**
 * @brief Node state in gossip protocol
 */
typedef enum {
    NODE_STATE_ALIVE = 0,
    NODE_STATE_SUSPECTED = 1,
    NODE_STATE_FAILED = 2,
    NODE_STATE_LEFT = 3
} node_state_t;

/**
 * @brief Node role
 */
typedef enum {
    NODE_ROLE_COORDINATOR = 0,
    NODE_ROLE_WORKER = 1
} node_role_t;

/**
 * @brief Task status
 */
typedef enum {
    TASK_STATUS_PENDING = 0,
    TASK_STATUS_RUNNING = 1,
    TASK_STATUS_COMPLETED = 2,
    TASK_STATUS_FAILED = 3,
    TASK_STATUS_TIMEOUT = 4,
    TASK_STATUS_CANCELLED = 5
} task_status_t;

/* ============================================================================
 * RAFT MESSAGE STRUCTURES
 * ========================================================================= */

/**
 * @brief Raft RequestVote RPC
 * 
 * Sent by candidates to gather votes during leader election.
 */
typedef struct {
    uint32_t term;                  /**< Candidate's term */
    char candidate_id[64];          /**< Candidate requesting vote */
    uint32_t last_log_index;        /**< Index of candidate's last log entry */
    uint32_t last_log_term;         /**< Term of candidate's last log entry */
} raft_request_vote_t;

/**
 * @brief Raft RequestVote Response
 */
typedef struct {
    uint32_t term;                  /**< Current term, for candidate to update itself */
    bool vote_granted;              /**< True if candidate received vote */
    char node_id[64];               /**< Responder's node ID */
} raft_request_vote_response_t;

/**
 * @brief Raft log entry (with entry_type field)
 */
typedef struct {
    uint32_t index;                 /**< Log entry index */
    uint32_t term;                  /**< Term when entry was received */
    raft_entry_type_t entry_type;   /**< Entry type (normal/config/noop) */
    uint8_t* data;                  /**< Entry data */
    size_t data_len;                /**< Data length */
} raft_log_entry_t;

/**
 * @brief Raft AppendEntries RPC
 * 
 * Sent by leader to replicate log entries and heartbeat.
 */
typedef struct {
    uint32_t term;                  /**< Leader's term */
    char leader_id[64];             /**< Leader's ID */
    uint32_t prev_log_index;        /**< Index of log entry preceding new ones */
    uint32_t prev_log_term;         /**< Term of prev_log_index entry */
    raft_log_entry_t* entries;      /**< Log entries to store (NULL for heartbeat) */
    size_t entry_count;             /**< Number of entries */
    uint32_t leader_commit;         /**< Leader's commit index */
} raft_append_entries_t;

/**
 * @brief Raft AppendEntries Response
 */
typedef struct {
    uint32_t term;                  /**< Current term, for leader to update itself */
    bool success;                   /**< True if follower contained entry matching prev_log_* */
    char node_id[64];               /**< Responder's node ID */
    uint32_t last_log_index;        /**< Follower's last log index (for optimization) */
} raft_append_entries_response_t;

/**
 * @brief Raft InstallSnapshot RPC
 * 
 * Sent by leader to bring slow followers up to date.
 */
typedef struct {
    uint32_t term;                  /**< Leader's term */
    char leader_id[64];             /**< Leader's ID */
    uint32_t last_included_index;   /**< Snapshot replaces all entries up through this index */
    uint32_t last_included_term;    /**< Term of last_included_index */
    uint32_t offset;                /**< Byte offset where chunk is positioned */
    uint8_t* data;                  /**< Raw bytes of snapshot chunk */
    size_t data_len;                /**< Data length */
    bool done;                      /**< True if this is the last chunk */
} raft_install_snapshot_t;

/**
 * @brief Raft InstallSnapshot Response
 */
typedef struct {
    uint32_t term;                  /**< Current term, for leader to update itself */
    char node_id[64];               /**< Responder's node ID */
    bool success;                   /**< True if snapshot accepted */
} raft_install_snapshot_response_t;

/**
 * @brief Node information for configuration
 */
typedef struct {
    char node_id[64];           /**< Node identifier */
    char address[256];          /**< Node address */
    uint16_t port;              /**< Node port */
    node_role_t role;           /**< Node role */
} raft_server_info_t;

/**
 * @brief Raft configuration change entry
 */
typedef struct {
    config_change_type_t type;  /**< Change type */
    
    /* Single node change */
    raft_server_info_t node_info;  /**< Node being added/removed */
    
    /* Joint consensus (for REPLACE type) */
    raft_server_info_t* old_servers;  /**< C_old configuration */
    size_t old_server_count;
    
    raft_server_info_t* new_servers;  /**< C_new configuration */
    size_t new_server_count;
} raft_configuration_change_t;

/* ============================================================================
 * GOSSIP MESSAGE STRUCTURES
 * ========================================================================= */

/**
 * @brief Node information for gossip (with load metrics)
 */
typedef struct {
    char node_id[64];               /**< Node identifier */
    char address[256];              /**< IP address or hostname */
    uint16_t port;                  /**< Port number */
    node_state_t state;             /**< Node state */
    node_role_t role;               /**< Node role */
    uint64_t incarnation;           /**< Incarnation number (for refutation) */
    
    /* Load metrics (0-100) */
    uint8_t cpu_usage;              /**< CPU usage percentage */
    uint8_t memory_usage;           /**< Memory usage percentage */
} gossip_node_info_t;

/**
 * @brief Gossip Ping message
 */
typedef struct {
    char sender_id[64];             /**< Sender's node ID */
    uint64_t incarnation;           /**< Sender's incarnation */
    uint32_t sequence_number;       /**< Ping sequence number */
} gossip_ping_t;

/**
 * @brief Gossip Ack message
 */
typedef struct {
    char sender_id[64];             /**< Responder's node ID */
    uint64_t incarnation;           /**< Responder's incarnation */
    uint32_t sequence_number;       /**< Ping sequence being acked */
} gossip_ack_t;

/**
 * @brief Gossip Indirect Ping message
 */
typedef struct {
    char sender_id[64];             /**< Original ping sender */
    char target_id[64];             /**< Target to ping */
    uint32_t sequence_number;       /**< Ping sequence number */
} gossip_indirect_ping_t;

/**
 * @brief Gossip Membership Update message
 */
typedef struct {
    char sender_id[64];             /**< Sender's node ID */
    gossip_node_info_t* updates;    /**< Array of node updates */
    size_t update_count;            /**< Number of updates */
} gossip_membership_update_t;

/* ============================================================================
 * TASK MESSAGE STRUCTURES
 * ========================================================================= */

/**
 * @brief Task Assignment message
 * 
 * Sent from coordinator to worker to assign a task.
 */
typedef struct {
    char task_id[128];              /**< Task identifier */
    char workflow_id[128];          /**< Workflow identifier */
    char task_type[64];             /**< Task type/plugin name */
    char* config_json;              /**< Task configuration (JSON string) */
    uint8_t* input_data;            /**< Task input data */
    size_t input_data_len;          /**< Input data length */
    uint32_t timeout_sec;           /**< Timeout in seconds */
    uint32_t retry_count;           /**< Number of retries allowed */
} task_assignment_t;

/**
 * @brief Task Result message
 * 
 * Sent from worker to coordinator with task results.
 */
typedef struct {
    char task_id[128];              /**< Task identifier */
    char worker_id[64];             /**< Worker that executed task */
    task_status_t status;           /**< Final task status */
    uint8_t* output_data;           /**< Task output data */
    size_t output_data_len;         /**< Output data length */
    char* error_message;            /**< Error message (if failed) */
    int32_t exit_code;              /**< Task exit code */
    uint64_t started_at;            /**< Start timestamp (ms) */
    uint64_t completed_at;          /**< Completion timestamp (ms) */
} task_result_t;

/**
 * @brief Task Status message
 * 
 * Query or update for task status.
 */
typedef struct {
    char task_id[128];              /**< Task identifier */
    task_status_t status;           /**< Current status */
    uint32_t retry_count;           /**< Current retry count */
    char* status_message;           /**< Status message */
} task_status_msg_t;

/* ============================================================================
 * CLIENT MESSAGE STRUCTURES
 * ========================================================================= */

/**
 * @brief Client Submit message
 * 
 * Submit a message for processing.
 */
typedef struct {
    char message_id[128];           /**< Message identifier */
    char event_type[64];            /**< Event type */
    char* payload_json;             /**< Message payload (JSON) */
    uint64_t timestamp;             /**< Submission timestamp */
} client_submit_t;

/**
 * @brief Client Response message
 */
typedef struct {
    char message_id[128];           /**< Original message ID */
    uint32_t response_code;         /**< Response code (200=success, etc.) */
    char* response_message;         /**< Human-readable message */
    char** workflows_triggered;     /**< Array of triggered workflow IDs */
    size_t workflow_count;          /**< Number of workflows triggered */
} client_response_t;

/**
 * @brief Client Query message
 */
typedef struct {
    char query_id[128];             /**< Query identifier */
    char query_type[64];            /**< Type of query */
    char* query_params_json;        /**< Query parameters (JSON) */
} client_query_t;

/**
 * @brief Client Error message
 */
typedef struct {
    char message_id[128];           /**< Related message ID */
    uint32_t error_code;            /**< Error code */
    char* error_message;            /**< Error description */
    char* error_details;            /**< Detailed error info */
} client_error_t;

/* ============================================================================
 * SERIALIZATION FUNCTIONS - RAFT
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
 * SERIALIZATION FUNCTIONS - GOSSIP
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
 * SERIALIZATION FUNCTIONS - TASK
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
 * SERIALIZATION FUNCTIONS - CLIENT
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