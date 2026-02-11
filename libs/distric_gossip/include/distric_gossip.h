/**
 * @file distric_gossip.h
 * @brief DistriC Gossip Protocol - SWIM-based Failure Detection
 * 
 * Implements SWIM (Scalable Weakly-consistent Infection-style Process Group
 * Membership) protocol for rapid failure detection across worker and coordinator nodes.
 * 
 * Thread Safety:
 * - All public APIs are thread-safe
 * - Callbacks may be invoked from internal threads
 * - User must ensure callback thread-safety
 * 
 * @version 1.0
 * @date 2026-02-11
 */

#ifndef DISTRIC_GOSSIP_H
#define DISTRIC_GOSSIP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "distric_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * TYPES AND STRUCTURES
 * ========================================================================= */

/**
 * @brief Node health status in the cluster
 */
typedef enum {
    GOSSIP_NODE_ALIVE = 0,      /**< Node is healthy and responding */
    GOSSIP_NODE_SUSPECTED,      /**< Node failed to respond, suspected dead */
    GOSSIP_NODE_FAILED,         /**< Node confirmed dead after timeout */
    GOSSIP_NODE_LEFT            /**< Node gracefully departed */
} gossip_node_status_t;

/**
 * @brief Node role in the DistriC cluster
 */
typedef enum {
    GOSSIP_ROLE_COORDINATOR = 0, /**< Coordination node (runs Raft) */
    GOSSIP_ROLE_WORKER          /**< Worker node (executes tasks) */
} gossip_node_role_t;

/**
 * @brief Information about a node in the gossip cluster
 */
typedef struct {
    char node_id[64];              /**< Unique node identifier */
    char address[256];             /**< IP address or hostname */
    uint16_t port;                 /**< UDP port for gossip */
    gossip_node_status_t status;   /**< Current health status */
    gossip_node_role_t role;       /**< Node role */
    uint64_t incarnation;          /**< Incarnation number (for conflict resolution) */
    uint64_t last_seen_ms;         /**< Last time node was seen (milliseconds) */
    uint64_t load;                 /**< Current load metric (0-100) */
} gossip_node_info_t;

/**
 * @brief Opaque handle to gossip state
 */
typedef struct gossip_state_s gossip_state_t;

/**
 * @brief Configuration for gossip protocol
 */
typedef struct {
    /* Node Identity */
    char node_id[64];              /**< This node's unique ID */
    char bind_address[256];        /**< Address to bind UDP socket */
    uint16_t bind_port;            /**< Port to bind UDP socket */
    gossip_node_role_t role;       /**< This node's role */
    
    /* Protocol Parameters */
    uint32_t protocol_period_ms;   /**< Gossip round interval (default: 1000ms) */
    uint32_t probe_timeout_ms;     /**< Timeout for direct probe (default: 500ms) */
    uint32_t indirect_probes;      /**< Number of indirect probes (default: 3) */
    uint32_t suspicion_mult;       /**< Multiplier for suspicion timeout (default: 3) */
    uint32_t max_transmissions;    /**< Max gossip transmissions per update (default: 3) */
    
    /* Seed Nodes */
    const char** seed_addresses;   /**< Array of seed node addresses */
    uint16_t* seed_ports;          /**< Array of seed node ports */
    size_t seed_count;             /**< Number of seed nodes */
    
    /* Observability */
    void* metrics;                 /**< Metrics registry (metrics_registry_t*) */
    void* logger;                  /**< Logger instance (logger_t*) */
} gossip_config_t;

/**
 * @brief Callback invoked when a node joins the cluster
 * 
 * @param node Information about the joined node
 * @param user_data User-provided context
 * 
 * Thread Safety: May be called from internal gossip thread
 */
typedef void (*gossip_node_joined_fn)(const gossip_node_info_t* node, void* user_data);

/**
 * @brief Callback invoked when a node is suspected of failure
 * 
 * @param node Information about the suspected node
 * @param user_data User-provided context
 * 
 * Thread Safety: May be called from internal gossip thread
 */
typedef void (*gossip_node_suspected_fn)(const gossip_node_info_t* node, void* user_data);

/**
 * @brief Callback invoked when a node is confirmed failed
 * 
 * @param node Information about the failed node
 * @param user_data User-provided context
 * 
 * Thread Safety: May be called from internal gossip thread
 */
typedef void (*gossip_node_failed_fn)(const gossip_node_info_t* node, void* user_data);

/**
 * @brief Callback invoked when a previously failed node recovers
 * 
 * @param node Information about the recovered node
 * @param user_data User-provided context
 * 
 * Thread Safety: May be called from internal gossip thread
 */
typedef void (*gossip_node_recovered_fn)(const gossip_node_info_t* node, void* user_data);

/* ============================================================================
 * LIFECYCLE FUNCTIONS
 * ========================================================================= */

/**
 * @brief Initialize gossip protocol state
 * 
 * Creates gossip state, initializes UDP socket, and prepares for joining cluster.
 * Does NOT start the gossip protocol (call gossip_start for that).
 * 
 * @param config Configuration parameters
 * @param state_out Output pointer for gossip state (must free with gossip_destroy)
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t gossip_init(const gossip_config_t* config, gossip_state_t** state_out);

/**
 * @brief Start the gossip protocol
 * 
 * Joins the cluster by contacting seed nodes and starts the gossip thread.
 * 
 * @param state Gossip state
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread (once per state)
 */
distric_err_t gossip_start(gossip_state_t* state);

/**
 * @brief Stop the gossip protocol
 * 
 * Stops the gossip thread but does NOT destroy state.
 * Can be restarted with gossip_start().
 * 
 * @param state Gossip state
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t gossip_stop(gossip_state_t* state);

/**
 * @brief Gracefully leave the cluster
 * 
 * Broadcasts a LEAVE message to all nodes and stops gossip.
 * 
 * @param state Gossip state
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t gossip_leave(gossip_state_t* state);

/**
 * @brief Destroy gossip state and free resources
 * 
 * Stops gossip if running and frees all memory.
 * 
 * @param state Gossip state (may be NULL)
 * 
 * Thread Safety: Safe to call from any thread (once per state)
 */
void gossip_destroy(gossip_state_t* state);

/* ============================================================================
 * MEMBERSHIP QUERY FUNCTIONS
 * ========================================================================= */

/**
 * @brief Get list of all alive nodes
 * 
 * Returns a snapshot of currently alive nodes. Caller must free the returned array.
 * 
 * @param state Gossip state
 * @param nodes_out Output array of node info (caller must free)
 * @param count_out Number of nodes in array
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t gossip_get_alive_nodes(
    gossip_state_t* state,
    gossip_node_info_t** nodes_out,
    size_t* count_out
);

/**
 * @brief Get list of alive nodes with specific role
 * 
 * @param state Gossip state
 * @param role Filter by this role
 * @param nodes_out Output array of node info (caller must free)
 * @param count_out Number of nodes in array
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t gossip_get_nodes_by_role(
    gossip_state_t* state,
    gossip_node_role_t role,
    gossip_node_info_t** nodes_out,
    size_t* count_out
);

/**
 * @brief Check if a specific node is alive
 * 
 * @param state Gossip state
 * @param node_id Node ID to check
 * @param is_alive_out Output: true if node is alive
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t gossip_is_node_alive(
    gossip_state_t* state,
    const char* node_id,
    bool* is_alive_out
);

/**
 * @brief Get information about a specific node
 * 
 * @param state Gossip state
 * @param node_id Node ID to query
 * @param info_out Output node information
 * @return DISTRIC_OK on success, DISTRIC_ERR_NOT_FOUND if node unknown
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t gossip_get_node_info(
    gossip_state_t* state,
    const char* node_id,
    gossip_node_info_t* info_out
);

/**
 * @brief Get total count of nodes by status
 * 
 * @param state Gossip state
 * @param status Filter by this status (or pass -1 for all)
 * @param count_out Output count
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t gossip_get_node_count(
    gossip_state_t* state,
    gossip_node_status_t status,
    size_t* count_out
);

/* ============================================================================
 * CALLBACK REGISTRATION
 * ========================================================================= */

/**
 * @brief Register callback for node join events
 * 
 * @param state Gossip state
 * @param callback Function to invoke on node join
 * @param user_data User context passed to callback
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call before gossip_start()
 */
distric_err_t gossip_set_on_node_joined(
    gossip_state_t* state,
    gossip_node_joined_fn callback,
    void* user_data
);

/**
 * @brief Register callback for node suspicion events
 * 
 * @param state Gossip state
 * @param callback Function to invoke when node suspected
 * @param user_data User context passed to callback
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call before gossip_start()
 */
distric_err_t gossip_set_on_node_suspected(
    gossip_state_t* state,
    gossip_node_suspected_fn callback,
    void* user_data
);

/**
 * @brief Register callback for node failure events
 * 
 * @param state Gossip state
 * @param callback Function to invoke when node fails
 * @param user_data User context passed to callback
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call before gossip_start()
 */
distric_err_t gossip_set_on_node_failed(
    gossip_state_t* state,
    gossip_node_failed_fn callback,
    void* user_data
);

/**
 * @brief Register callback for node recovery events
 * 
 * @param state Gossip state
 * @param callback Function to invoke when node recovers
 * @param user_data User context passed to callback
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call before gossip_start()
 */
distric_err_t gossip_set_on_node_recovered(
    gossip_state_t* state,
    gossip_node_recovered_fn callback,
    void* user_data
);

/* ============================================================================
 * STATE MODIFICATION
 * ========================================================================= */

/**
 * @brief Update this node's load metric
 * 
 * Updates the local load value that will be broadcast to other nodes.
 * 
 * @param state Gossip state
 * @param load Load value (0-100)
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t gossip_update_load(gossip_state_t* state, uint64_t load);

/**
 * @brief Refute a suspicion against this node
 * 
 * Increments incarnation number and broadcasts ALIVE message.
 * Called automatically when this node is suspected, but can be called manually.
 * 
 * @param state Gossip state
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t gossip_refute_suspicion(gossip_state_t* state);

/**
 * @brief Add a new seed node dynamically
 * 
 * @param state Gossip state
 * @param address Seed node address
 * @param port Seed node port
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t gossip_add_seed(
    gossip_state_t* state,
    const char* address,
    uint16_t port
);

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

/**
 * @brief Get human-readable status string
 * 
 * @param status Status enum value
 * @return String representation (do not free)
 */
const char* gossip_status_to_string(gossip_node_status_t status);

/**
 * @brief Get human-readable role string
 * 
 * @param role Role enum value
 * @return String representation (do not free)
 */
const char* gossip_role_to_string(gossip_node_role_t role);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_GOSSIP_H */