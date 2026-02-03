/**
 * @file raft_core.h
 * @brief Raft Consensus Core - Leader Election and Log Replication
 * 
 * Implements the Raft consensus algorithm for distributed coordination.
 * Based on "In Search of an Understandable Consensus Algorithm" (Extended Version)
 * by Diego Ongaro and John Ousterhout.
 * 
 * Key Features:
 * - Leader election with randomized timeouts
 * - Log replication with majority quorum
 * - Safety guarantees (Leader Completeness, State Machine Safety)
 * - Log compaction via snapshotting
 * - Dynamic membership changes
 * 
 * @version 1.0.0
 */

#ifndef DISTRIC_RAFT_CORE_H
#define DISTRIC_RAFT_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <distric_obs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * RAFT STATE
 * ========================================================================= */

/**
 * @brief Raft node state
 */
typedef enum {
    RAFT_STATE_FOLLOWER = 0,
    RAFT_STATE_CANDIDATE = 1,
    RAFT_STATE_LEADER = 2
} raft_state_t;

/**
 * @brief Raft log entry type
 */
typedef enum {
    RAFT_ENTRY_COMMAND = 0,     /**< Normal state machine command */
    RAFT_ENTRY_CONFIG = 1,      /**< Configuration change */
    RAFT_ENTRY_NOOP = 2         /**< No-op (leader election marker) */
} raft_entry_type_t;

/**
 * @brief Raft log entry
 */
typedef struct {
    uint32_t index;             /**< Log index (1-based) */
    uint32_t term;              /**< Term when entry was received */
    raft_entry_type_t type;     /**< Entry type */
    uint8_t* data;              /**< Entry data (owned by log) */
    size_t data_len;            /**< Data length */
} raft_log_entry_t;

/**
 * @brief Raft peer state (leader's view)
 */
typedef struct {
    char node_id[64];           /**< Peer node ID */
    char address[256];          /**< Peer address */
    uint16_t port;              /**< Peer port */
    
    /* Leader state (valid only when we're leader) */
    uint32_t next_index;        /**< Next log entry to send */
    uint32_t match_index;       /**< Highest log entry known to be replicated */
    
    /* Heartbeat tracking */
    uint64_t last_contact_ms;   /**< Last successful RPC */
    bool vote_granted;          /**< Vote granted in current term */
} raft_peer_t;

/* Forward declarations */
typedef struct raft_node raft_node_t;
typedef struct raft_config raft_config_t;

/**
 * @brief Callback for applying committed entries to state machine
 * 
 * @param entry Log entry to apply
 * @param user_data User context
 */
typedef void (*raft_apply_fn_t)(const raft_log_entry_t* entry, void* user_data);

/**
 * @brief Callback for state transitions
 * 
 * @param old_state Previous state
 * @param new_state New state
 * @param user_data User context
 */
typedef void (*raft_state_change_fn_t)(raft_state_t old_state, raft_state_t new_state, void* user_data);

/**
 * @brief Raft configuration
 */
struct raft_config {
    /* Node identity */
    char node_id[64];           /**< This node's ID */
    
    /* Cluster configuration */
    raft_peer_t* peers;         /**< Array of peer nodes */
    size_t peer_count;          /**< Number of peers */
    
    /* Timing configuration */
    uint32_t election_timeout_min_ms;  /**< Min election timeout (default: 150) */
    uint32_t election_timeout_max_ms;  /**< Max election timeout (default: 300) */
    uint32_t heartbeat_interval_ms;    /**< Heartbeat interval (default: 50) */
    
    /* Log configuration */
    uint32_t snapshot_threshold;       /**< Snapshot after N log entries (default: 10000) */
    
    /* Callbacks */
    raft_apply_fn_t apply_fn;           /**< State machine apply callback */
    raft_state_change_fn_t state_change_fn;  /**< State change callback */
    void* user_data;                    /**< User context for callbacks */
    
    /* Observability */
    metrics_registry_t* metrics;
    logger_t* logger;
};

/* ============================================================================
 * RAFT NODE API
 * ========================================================================= */

distric_err_t raft_create(const raft_config_t* config, raft_node_t** node_out);
void raft_destroy(raft_node_t* node);
distric_err_t raft_start(raft_node_t* node);
distric_err_t raft_stop(raft_node_t* node);
distric_err_t raft_tick(raft_node_t* node);

/* ============================================================================
 * STATE QUERIES
 * ========================================================================= */

raft_state_t raft_get_state(const raft_node_t* node);
bool raft_is_leader(const raft_node_t* node);
uint32_t raft_get_term(const raft_node_t* node);
distric_err_t raft_get_leader(const raft_node_t* node, char* leader_id_out);
uint32_t raft_get_commit_index(const raft_node_t* node);
uint32_t raft_get_last_log_index(const raft_node_t* node);

/* ============================================================================
 * LOG OPERATIONS (Leader Only)
 * ========================================================================= */

distric_err_t raft_append_entry(
    raft_node_t* node,
    const uint8_t* data,
    size_t data_len,
    uint32_t* index_out
);

distric_err_t raft_wait_committed(
    raft_node_t* node,
    uint32_t index,
    uint32_t timeout_ms
);

/* ============================================================================
 * RPC HANDLERS (Called by network layer)
 * ========================================================================= */

distric_err_t raft_handle_request_vote(
    raft_node_t* node,
    const char* candidate_id,
    uint32_t term,
    uint32_t last_log_index,
    uint32_t last_log_term,
    bool* vote_granted_out,
    uint32_t* term_out
);

distric_err_t raft_handle_append_entries(
    raft_node_t* node,
    const char* leader_id,
    uint32_t term,
    uint32_t prev_log_index,
    uint32_t prev_log_term,
    const raft_log_entry_t* entries,
    size_t entry_count,
    uint32_t leader_commit,
    bool* success_out,
    uint32_t* term_out,
    uint32_t* last_log_index_out
);

/* ============================================================================
 * SNAPSHOT API
 * ========================================================================= */

distric_err_t raft_create_snapshot(
    raft_node_t* node,
    const uint8_t* snapshot_data,
    size_t snapshot_len
);

distric_err_t raft_install_snapshot(
    raft_node_t* node,
    uint32_t last_included_index,
    uint32_t last_included_term,
    const uint8_t* snapshot_data,
    size_t snapshot_len
);

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

const char* raft_state_to_string(raft_state_t state);

/* ============================================================================
 * RPC HELPER FUNCTIONS (Added in Session 3.2)
 * ========================================================================= */

const raft_config_t* raft_get_config(const raft_node_t* node);
const char* raft_get_node_id(const raft_node_t* node);
uint32_t raft_get_last_log_term(const raft_node_t* node);
uint32_t raft_get_log_term(const raft_node_t* node, uint32_t index);

distric_err_t raft_get_log_entries(
    const raft_node_t* node,
    uint32_t start_index,
    uint32_t end_index,
    raft_log_entry_t** entries_out,
    size_t* count_out
);

uint32_t raft_get_peer_next_index(const raft_node_t* node, size_t peer_index);

distric_err_t raft_update_peer_indices(
    raft_node_t* node,
    size_t peer_index,
    uint32_t next_index,
    uint32_t match_index
);

distric_err_t raft_decrement_peer_next_index(raft_node_t* node, size_t peer_index);
distric_err_t raft_step_down(raft_node_t* node, uint32_t term);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_RAFT_CORE_H */