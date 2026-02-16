/**
 * @file raft_rpc.h
 * @brief Raft RPC Integration - Network Communication Layer
 * 
 * Provides RPC client and server functionality for Raft consensus:
 * - RequestVote RPC (send/receive)
 * - AppendEntries RPC (send/receive)
 * - InstallSnapshot RPC (send/receive)
 * - Automatic retry and timeout handling
 * - Integration with distric_protocol RPC framework
 * - Send filter support for testing (network partitions)
 * 
 * @version 1.0.0
 */

#ifndef DISTRIC_RAFT_RPC_H
#define DISTRIC_RAFT_RPC_H

#include <distric_raft/raft_core.h>
#include <distric_protocol.h>
#include <distric_transport.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * RAFT RPC CONTEXT
 * ========================================================================= */

/**
 * @brief Raft RPC context (one per Raft node)
 * 
 * Manages RPC client and server for Raft communication.
 */
typedef struct raft_rpc_context raft_rpc_context_t;

/**
 * @brief Send filter callback
 * 
 * Allows filtering outgoing RPC messages. Used for testing network
 * partitions and message loss simulation.
 * 
 * @param userdata User-provided context
 * @param peer_address Target peer address
 * @param peer_port Target peer port
 * @return true to allow send, false to block
 */
typedef bool (*raft_rpc_send_filter_t)(
    void* userdata,
    const char* peer_address,
    uint16_t peer_port
);

/**
 * @brief RPC configuration
 */
typedef struct {
    const char* bind_address;       /**< Address to bind RPC server (e.g., "0.0.0.0") */
    uint16_t bind_port;             /**< Port to bind RPC server */
    
    uint32_t rpc_timeout_ms;        /**< RPC timeout in milliseconds (default: 1000) */
    uint32_t max_retries;           /**< Max RPC retry attempts (default: 3) */
    
    metrics_registry_t* metrics;    /**< Metrics registry */
    logger_t* logger;               /**< Logger */
} raft_rpc_config_t;

/* ============================================================================
 * RPC CONTEXT LIFECYCLE
 * ========================================================================= */

/**
 * @brief Create Raft RPC context
 * 
 * Initializes RPC client and server for Raft communication.
 * The RPC server will handle incoming RequestVote and AppendEntries RPCs.
 * 
 * @param config RPC configuration
 * @param raft_node Associated Raft node
 * @param context_out Output: created RPC context
 * @return DISTRIC_OK on success
 */
distric_err_t raft_rpc_create(
    const raft_rpc_config_t* config,
    raft_node_t* raft_node,
    raft_rpc_context_t** context_out
);

/**
 * @brief Start RPC server
 * 
 * Begins accepting incoming RPCs.
 * 
 * @param context RPC context
 * @return DISTRIC_OK on success
 */
distric_err_t raft_rpc_start(raft_rpc_context_t* context);

/**
 * @brief Stop RPC server
 * 
 * Stops accepting incoming RPCs.
 * 
 * @param context RPC context
 * @return DISTRIC_OK on success
 */
distric_err_t raft_rpc_stop(raft_rpc_context_t* context);

/**
 * @brief Destroy RPC context
 * 
 * Frees all resources.
 * 
 * @param context RPC context
 */
void raft_rpc_destroy(raft_rpc_context_t* context);

/* ============================================================================
 * SEND FILTER (FOR TESTING)
 * ========================================================================= */

/**
 * @brief Set send filter callback
 * 
 * Registers a callback that will be invoked before sending each RPC.
 * If the callback returns false, the message is blocked.
 * 
 * This is primarily used for testing network partitions and message loss.
 * 
 * @param context RPC context
 * @param filter Filter callback (NULL to disable)
 * @param userdata User data passed to filter callback
 */
void raft_rpc_set_send_filter(
    raft_rpc_context_t* context,
    raft_rpc_send_filter_t filter,
    void* userdata
);

/* ============================================================================
 * RPC CLIENT OPERATIONS
 * ========================================================================= */

/**
 * @brief Send RequestVote RPC to peer
 * 
 * Sends RequestVote RPC and waits for response.
 * Automatically retries on failure according to config.
 * 
 * Note: If a send filter is registered and blocks the message,
 * this function returns DISTRIC_ERR_UNAVAILABLE.
 * 
 * @param context RPC context
 * @param peer_address Peer address (e.g., "10.0.1.5")
 * @param peer_port Peer port
 * @param candidate_id Candidate node ID
 * @param term Candidate's term
 * @param last_log_index Candidate's last log index
 * @param last_log_term Candidate's last log term
 * @param vote_granted_out Output: vote granted
 * @param term_out Output: peer's current term
 * @return DISTRIC_OK on success, DISTRIC_ERR_UNAVAILABLE if blocked by filter
 */
distric_err_t raft_rpc_send_request_vote(
    raft_rpc_context_t* context,
    const char* peer_address,
    uint16_t peer_port,
    const char* candidate_id,
    uint32_t term,
    uint32_t last_log_index,
    uint32_t last_log_term,
    bool* vote_granted_out,
    uint32_t* term_out
);

/**
 * @brief Send AppendEntries RPC to peer
 * 
 * Sends AppendEntries RPC (heartbeat or log replication) and waits for response.
 * 
 * Note: If a send filter is registered and blocks the message,
 * this function returns DISTRIC_ERR_UNAVAILABLE.
 * 
 * @param context RPC context
 * @param peer_address Peer address
 * @param peer_port Peer port
 * @param leader_id Leader node ID
 * @param term Leader's term
 * @param prev_log_index Previous log index
 * @param prev_log_term Previous log term
 * @param entries Log entries (NULL for heartbeat)
 * @param entry_count Number of entries
 * @param leader_commit Leader's commit index
 * @param success_out Output: true if follower matched prev_log
 * @param term_out Output: peer's current term
 * @param match_index_out Output: peer's last log index (for updating match_index)
 * @return DISTRIC_OK on success, DISTRIC_ERR_UNAVAILABLE if blocked by filter
 */
distric_err_t raft_rpc_send_append_entries(
    raft_rpc_context_t* context,
    const char* peer_address,
    uint16_t peer_port,
    const char* leader_id,
    uint32_t term,
    uint32_t prev_log_index,
    uint32_t prev_log_term,
    const raft_log_entry_t* entries,
    size_t entry_count,
    uint32_t leader_commit,
    bool* success_out,
    uint32_t* term_out,
    uint32_t* match_index_out
);

/**
 * @brief Send InstallSnapshot RPC to peer
 * 
 * Sends snapshot to follower that is too far behind.
 * 
 * Note: If a send filter is registered and blocks the message,
 * this function returns DISTRIC_ERR_UNAVAILABLE.
 * 
 * @param context RPC context
 * @param peer_address Peer address
 * @param peer_port Peer port
 * @param leader_id Leader node ID
 * @param term Leader's term
 * @param last_included_index Snapshot's last included log index
 * @param last_included_term Snapshot's last included term
 * @param snapshot_data Snapshot data
 * @param snapshot_len Snapshot length
 * @param success_out Output: true if accepted
 * @param term_out Output: peer's current term
 * @return DISTRIC_OK on success, DISTRIC_ERR_UNAVAILABLE if blocked by filter
 */
distric_err_t raft_rpc_send_install_snapshot(
    raft_rpc_context_t* context,
    const char* peer_address,
    uint16_t peer_port,
    const char* leader_id,
    uint32_t term,
    uint32_t last_included_index,
    uint32_t last_included_term,
    const uint8_t* snapshot_data,
    size_t snapshot_len,
    bool* success_out,
    uint32_t* term_out
);

/**
 * @brief Broadcast RequestVote to all peers
 * 
 * Sends RequestVote RPC to all peers in parallel.
 * 
 * @param context RPC context
 * @param raft_node Raft node (for getting peer list)
 * @param term Current term
 * @param last_log_index Last log index
 * @param last_log_term Last log term
 * @param votes_received_out Output: number of votes received
 * @return DISTRIC_OK on success
 */
distric_err_t raft_rpc_broadcast_request_vote(
    raft_rpc_context_t* context,
    raft_node_t* raft_node,
    uint32_t term,
    uint32_t last_log_index,
    uint32_t last_log_term,
    uint32_t* votes_received_out
);

/**
 * @brief Broadcast AppendEntries to all peers
 * 
 * Sends AppendEntries RPC (heartbeat or replication) to all peers in parallel.
 * 
 * @param context RPC context
 * @param raft_node Raft node (for getting peer list and per-peer state)
 * @return DISTRIC_OK on success
 */
distric_err_t raft_rpc_broadcast_append_entries(
    raft_rpc_context_t* context,
    raft_node_t* raft_node
);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_RAFT_RPC_H */