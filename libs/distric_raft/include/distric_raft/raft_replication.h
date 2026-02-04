/**
 * @file raft_replication.h
 * @brief Raft Log Replication API
 * 
 * Provides functions for log replication management.
 * These functions are called by the RPC layer to implement
 * leader-driven log replication.
 * 
 * @version 1.0.0
 */

#ifndef DISTRIC_RAFT_REPLICATION_H
#define DISTRIC_RAFT_REPLICATION_H

#include <distric_raft/raft_core.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * REPLICATION CONTROL
 * ========================================================================= */

/**
 * @brief Check if leader should replicate to peer
 * 
 * Returns true if peer has entries to replicate or needs heartbeat.
 * 
 * @param node Raft node
 * @param peer_index Peer index in config.peers array
 * @return true if replication needed
 */
bool raft_should_replicate_to_peer(raft_node_t* node, size_t peer_index);

/**
 * @brief Check if it's time to send heartbeat
 * 
 * @param node Raft node
 * @return true if heartbeat should be sent
 */
bool raft_should_send_heartbeat(raft_node_t* node);

/**
 * @brief Mark heartbeat as sent
 * 
 * Updates last_heartbeat_sent_ms timestamp.
 * 
 * @param node Raft node
 */
void raft_mark_heartbeat_sent(raft_node_t* node);

/* ============================================================================
 * RESPONSE HANDLING
 * ========================================================================= */

/**
 * @brief Handle AppendEntries response from follower
 * 
 * Updates next_index and match_index based on response.
 * Advances commit_index if majority has replicated.
 * 
 * @param node Raft node
 * @param peer_index Peer index
 * @param success True if follower accepted entries
 * @param peer_term Follower's current term
 * @param match_index Follower's last log index (on success)
 * @return DISTRIC_OK on success
 */
distric_err_t raft_handle_append_entries_response(
    raft_node_t* node,
    size_t peer_index,
    bool success,
    uint32_t peer_term,
    uint32_t match_index
);

/* ============================================================================
 * ENTRY RETRIEVAL
 * ========================================================================= */

/**
 * @brief Get entries to replicate to peer
 * 
 * Returns entries from next_index onwards, up to batch size limit.
 * Also returns prev_log_index and prev_log_term for consistency check.
 * 
 * @param node Raft node
 * @param peer_index Peer index
 * @param entries_out Output: array of log entries (caller must free)
 * @param count_out Output: number of entries
 * @param prev_log_index_out Output: index of entry before first entry
 * @param prev_log_term_out Output: term of prev_log_index
 * @return DISTRIC_OK on success
 */
distric_err_t raft_get_entries_for_peer(
    raft_node_t* node,
    size_t peer_index,
    raft_log_entry_t** entries_out,
    size_t* count_out,
    uint32_t* prev_log_index_out,
    uint32_t* prev_log_term_out
);

/**
 * @brief Free entries returned by raft_get_entries_for_peer
 * 
 * @param entries Array of entries
 * @param count Number of entries
 */
void raft_free_log_entries(raft_log_entry_t* entries, size_t count);

/* ============================================================================
 * CONFLICT RESOLUTION
 * ========================================================================= */

/**
 * @brief Handle log conflict with follower
 * 
 * Optimally adjusts next_index based on conflict information.
 * 
 * @param node Raft node
 * @param peer_index Peer index
 * @param conflict_index Index where conflict occurred
 * @param conflict_term Term of conflicting entry (0 if unknown)
 * @return DISTRIC_OK on success
 */
distric_err_t raft_handle_log_conflict(
    raft_node_t* node,
    size_t peer_index,
    uint32_t conflict_index,
    uint32_t conflict_term
);

/* ============================================================================
 * COMMIT WAITING
 * ========================================================================= */

/**
 * @brief Wait for entry to be committed
 * 
 * Blocks until entry at given index is committed or timeout expires.
 * 
 * @param node Raft node
 * @param index Log index to wait for
 * @param timeout_ms Timeout in milliseconds
 * @return DISTRIC_OK if committed, DISTRIC_ERR_TIMEOUT on timeout
 */
distric_err_t raft_wait_committed(
    raft_node_t* node,
    uint32_t index,
    uint32_t timeout_ms
);

/* ============================================================================
 * MONITORING
 * ========================================================================= */

/**
 * @brief Get replication lag for peer
 * 
 * Returns number of entries peer is behind leader.
 * 
 * @param node Raft node
 * @param peer_index Peer index
 * @return Number of entries behind (0 if up-to-date or error)
 */
uint32_t raft_get_peer_lag(raft_node_t* node, size_t peer_index);

/**
 * @brief Replication statistics
 */
typedef struct {
    uint32_t last_log_index;    /**< Leader's last log index */
    uint32_t commit_index;      /**< Leader's commit index */
    uint32_t min_match_index;   /**< Minimum match_index across peers */
    uint32_t max_match_index;   /**< Maximum match_index across peers */
    uint32_t peers_up_to_date;  /**< Number of peers up-to-date */
    uint32_t peers_lagging;     /**< Number of peers lagging */
} replication_stats_t;

/**
 * @brief Get replication progress summary
 * 
 * @param node Raft node (must be leader)
 * @param stats_out Output: replication statistics
 * @return DISTRIC_OK on success
 */
distric_err_t raft_get_replication_stats(
    raft_node_t* node,
    replication_stats_t* stats_out
);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_RAFT_REPLICATION_H */