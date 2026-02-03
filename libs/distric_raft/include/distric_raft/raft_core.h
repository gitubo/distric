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

/**
 * @brief Create a new Raft node
 * 
 * @param config Raft configuration
 * @param node_out Output: created node
 * @return DISTRIC_OK on success
 */
distric_err_t raft_create(const raft_config_t* config, raft_node_t** node_out);

/**
 * @brief Destroy Raft node and free all resources
 * 
 * @param node Raft node
 */
void raft_destroy(raft_node_t* node);

/**
 * @brief Start Raft node (begin election timer)
 * 
 * @param node Raft node
 * @return DISTRIC_OK on success
 */
distric_err_t raft_start(raft_node_t* node);

/**
 * @brief Stop Raft node gracefully
 * 
 * @param node Raft node
 * @return DISTRIC_OK on success
 */
distric_err_t raft_stop(raft_node_t* node);

/**
 * @brief Periodic tick (drives timers and heartbeats)
 * 
 * Should be called at regular intervals (e.g., every 10ms).
 * Handles election timeouts, heartbeats, and log replication.
 * 
 * @param node Raft node
 * @return DISTRIC_OK on success
 */
distric_err_t raft_tick(raft_node_t* node);

/* ============================================================================
 * STATE QUERIES
 * ========================================================================= */

/**
 * @brief Get current Raft state
 * 
 * @param node Raft node
 * @return Current state (FOLLOWER/CANDIDATE/LEADER)
 */
raft_state_t raft_get_state(const raft_node_t* node);

/**
 * @brief Check if this node is the leader
 * 
 * @param node Raft node
 * @return true if leader
 */
bool raft_is_leader(const raft_node_t* node);

/**
 * @brief Get current term
 * 
 * @param node Raft node
 * @return Current term
 */
uint32_t raft_get_term(const raft_node_t* node);

/**
 * @brief Get current leader ID
 * 
 * @param node Raft node
 * @param leader_id_out Output buffer for leader ID (64 bytes)
 * @return DISTRIC_OK if leader is known, DISTRIC_ERR_NOT_FOUND otherwise
 */
distric_err_t raft_get_leader(const raft_node_t* node, char* leader_id_out);

/**
 * @brief Get commit index
 * 
 * @param node Raft node
 * @return Commit index
 */
uint32_t raft_get_commit_index(const raft_node_t* node);

/**
 * @brief Get last log index
 * 
 * @param node Raft node
 * @return Last log index (0 if empty)
 */
uint32_t raft_get_last_log_index(const raft_node_t* node);

/* ============================================================================
 * LOG OPERATIONS (Leader Only)
 * ========================================================================= */

/**
 * @brief Append entry to log (leader only)
 * 
 * The entry is appended to the local log and will be replicated
 * to followers. The entry is committed when replicated to a majority.
 * 
 * @param node Raft node (must be leader)
 * @param data Entry data (will be copied)
 * @param data_len Data length
 * @param index_out Output: index of appended entry
 * @return DISTRIC_OK on success, DISTRIC_ERR_NOT_FOUND if not leader
 */
distric_err_t raft_append_entry(
    raft_node_t* node,
    const uint8_t* data,
    size_t data_len,
    uint32_t* index_out
);

/**
 * @brief Wait for entry to be committed
 * 
 * Blocks until the entry at the given index is committed or timeout expires.
 * 
 * @param node Raft node
 * @param index Log index to wait for
 * @param timeout_ms Timeout in milliseconds (0 = no wait)
 * @return DISTRIC_OK if committed, DISTRIC_ERR_TIMEOUT if timeout
 */
distric_err_t raft_wait_committed(
    raft_node_t* node,
    uint32_t index,
    uint32_t timeout_ms
);

/* ============================================================================
 * RPC HANDLERS (Called by network layer)
 * ========================================================================= */

/**
 * @brief Handle RequestVote RPC
 * 
 * @param node Raft node
 * @param candidate_id Candidate's node ID
 * @param term Candidate's term
 * @param last_log_index Candidate's last log index
 * @param last_log_term Candidate's last log term
 * @param vote_granted_out Output: vote granted
 * @param term_out Output: current term
 * @return DISTRIC_OK on success
 */
distric_err_t raft_handle_request_vote(
    raft_node_t* node,
    const char* candidate_id,
    uint32_t term,
    uint32_t last_log_index,
    uint32_t last_log_term,
    bool* vote_granted_out,
    uint32_t* term_out
);

/**
 * @brief Handle AppendEntries RPC
 * 
 * @param node Raft node
 * @param leader_id Leader's node ID
 * @param term Leader's term
 * @param prev_log_index Previous log index
 * @param prev_log_term Previous log term
 * @param entries Log entries (NULL for heartbeat)
 * @param entry_count Number of entries
 * @param leader_commit Leader's commit index
 * @param success_out Output: true if follower contained matching prev_log
 * @param term_out Output: current term
 * @param last_log_index_out Output: follower's last log index
 * @return DISTRIC_OK on success
 */
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

/**
 * @brief Create snapshot of current state
 * 
 * @param node Raft node
 * @param snapshot_data Snapshot data
 * @param snapshot_len Snapshot length
 * @return DISTRIC_OK on success
 */
distric_err_t raft_create_snapshot(
    raft_node_t* node,
    const uint8_t* snapshot_data,
    size_t snapshot_len
);

/**
 * @brief Install snapshot (received from leader)
 * 
 * @param node Raft node
 * @param last_included_index Last index in snapshot
 * @param last_included_term Last term in snapshot
 * @param snapshot_data Snapshot data
 * @param snapshot_len Snapshot length
 * @return DISTRIC_OK on success
 */
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

/**
 * @brief Get string representation of Raft state
 * 
 * @param state Raft state
 * @return State string ("FOLLOWER"/"CANDIDATE"/"LEADER")
 */
const char* raft_state_to_string(raft_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_RAFT_CORE_H */