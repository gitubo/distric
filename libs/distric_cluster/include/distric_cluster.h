#ifndef DISTRIC_CLUSTER_H
#define DISTRIC_CLUSTER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "distric_obs.h"
#include "distric_raft.h"
#include "distric_gossip.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct cluster_coordinator_t cluster_coordinator_t;
typedef struct cluster_node_t cluster_node_t;

/* ====================================================================
 * Enums
 * ==================================================================== */

/**
 * Cluster coordinator state
 */
typedef enum {
    CLUSTER_STATE_UNKNOWN = 0,
    CLUSTER_STATE_STOPPED,
    CLUSTER_STATE_STARTING,
    CLUSTER_STATE_RUNNING,
    CLUSTER_STATE_STOPPING,
    CLUSTER_STATE_ERROR,
    CLUSTER_STATE_DEGRADED,
    CLUSTER_STATE_ACTIVE,
    CLUSTER_STATE_SHUTDOWN
} cluster_state_t;

/**
 * Node type in the cluster
 */
typedef enum {
    CLUSTER_NODE_UNKNOWN = 0,
    CLUSTER_NODE_COORDINATOR,
    CLUSTER_NODE_WORKER
} cluster_node_type_t;

/* ====================================================================
 * Structs
 * ==================================================================== */

/**
 * Cluster node information
 */
struct cluster_node_t {
    char node_id[64];
    char address[256];
    uint16_t gossip_port;
    cluster_node_type_t node_type;
    node_state_t gossip_status;
    uint64_t last_seen_ms;
    uint32_t load;
};

/**
 * Cluster configuration
 */
typedef struct {
    /* Node identity */
    char node_id[64];
    cluster_node_type_t node_type;
    char bind_address[256];
    
    /* Gossip protocol settings */
    uint16_t gossip_port;
    uint32_t gossip_interval_ms;
    char** gossip_seeds;
    size_t gossip_seed_count;
    
    /* Raft consensus settings (for coordinators) */
    uint32_t raft_election_timeout_min_ms;
    uint32_t raft_election_timeout_max_ms;
    uint32_t raft_heartbeat_interval_ms;
    char** raft_peers;
    size_t raft_peer_count;
    
    /* Storage */
    char storage_path[512];
    
    /* Timing */
    uint32_t heartbeat_interval_ms;
    uint32_t election_timeout_ms;
    
    /* Dependencies */
    metrics_registry_t* metrics;
    logger_t* logger;
} cluster_config_t;

/* ====================================================================
 * Callback Function Types
 * ==================================================================== */

/**
 * Callback when this coordinator becomes the leader
 * 
 * @param coord The cluster coordinator
 * @param user_data User-provided callback data
 */
typedef void (*cluster_on_became_leader_fn)(
    cluster_coordinator_t* coord,
    void* user_data
);

/**
 * Callback when this coordinator loses leadership
 * 
 * @param coord The cluster coordinator
 * @param user_data User-provided callback data
 */
typedef void (*cluster_on_lost_leadership_fn)(
    cluster_coordinator_t* coord,
    void* user_data
);

/**
 * Callback when a worker node joins the cluster
 * 
 * @param coord The cluster coordinator
 * @param node Information about the joined node
 * @param user_data User-provided callback data
 */
typedef void (*cluster_on_worker_joined_fn)(
    cluster_coordinator_t* coord,
    const cluster_node_t* node,
    void* user_data
);

/**
 * Callback when a worker node fails
 * 
 * @param coord The cluster coordinator
 * @param node Information about the failed node
 * @param user_data User-provided callback data
 */
typedef void (*cluster_on_worker_failed_fn)(
    cluster_coordinator_t* coord,
    const cluster_node_t* node,
    void* user_data
);

/* ====================================================================
 * Core Functions
 * ==================================================================== */

/**
 * Create a new cluster coordinator
 * 
 * @param config Configuration for the cluster
 * @param coord_out Output parameter for the created coordinator
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t cluster_coordinator_create(
    const cluster_config_t* config,
    cluster_coordinator_t** coord_out
);

/**
 * Start the cluster coordinator
 * 
 * @param coord The cluster coordinator
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t cluster_coordinator_start(cluster_coordinator_t* coord);

/**
 * Stop the cluster coordinator
 * 
 * @param coord The cluster coordinator
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t cluster_coordinator_stop(cluster_coordinator_t* coord);

/**
 * Destroy the cluster coordinator and free resources
 * 
 * @param coord The cluster coordinator to destroy
 */
void cluster_coordinator_destroy(cluster_coordinator_t* coord);

/**
 * Get the current state of the cluster coordinator
 * 
 * @param coord The cluster coordinator
 * @return Current cluster state
 */
cluster_state_t cluster_coordinator_get_state(const cluster_coordinator_t* coord);

/**
 * Check if this coordinator is the current leader
 * 
 * @param coord The cluster coordinator
 * @return true if this node is the leader, false otherwise
 */
bool cluster_coordinator_is_leader(const cluster_coordinator_t* coord);

/* ====================================================================
 * Callback Registration
 * ==================================================================== */

/**
 * Set leadership change callbacks
 * 
 * @param coord The cluster coordinator
 * @param on_became_leader Callback when becoming leader (can be NULL)
 * @param on_lost_leadership Callback when losing leadership (can be NULL)
 * @param user_data User data passed to callbacks
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t cluster_coordinator_set_leadership_callbacks(
    cluster_coordinator_t* coord,
    cluster_on_became_leader_fn on_became_leader,
    cluster_on_lost_leadership_fn on_lost_leadership,
    void* user_data
);

/**
 * Set worker node event callbacks
 * 
 * @param coord The cluster coordinator
 * @param on_worker_joined Callback when worker joins (can be NULL)
 * @param on_worker_failed Callback when worker fails (can be NULL)
 * @param user_data User data passed to callbacks
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t cluster_coordinator_set_worker_callbacks(
    cluster_coordinator_t* coord,
    cluster_on_worker_joined_fn on_worker_joined,
    cluster_on_worker_failed_fn on_worker_failed,
    void* user_data
);

/* ====================================================================
 * Worker Node Management
 * ==================================================================== */

/**
 * Get list of active worker nodes
 * 
 * @param coord The cluster coordinator
 * @param nodes_out Output array of nodes (caller must free)
 * @param count_out Number of nodes in the array
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t cluster_coordinator_get_workers(
    cluster_coordinator_t* coord,
    cluster_node_t** nodes_out,
    size_t* count_out
);

/**
 * Submit a task to a worker node
 * 
 * @param coord The cluster coordinator
 * @param node_id Target worker node ID (NULL for automatic selection)
 * @param node_type Type of node to target
 * @param task_data Task data to submit
 * @param task_size Size of task data
 * @param task_id_out Output parameter for assigned task ID
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t cluster_coordinator_submit_task(
    cluster_coordinator_t* coord,
    const char* node_id,
    cluster_node_type_t node_type,
    const void* task_data,
    size_t task_size,
    uint64_t* task_id_out
);

/* ====================================================================
 * Utility Functions
 * ==================================================================== */

/**
 * Convert cluster state to string representation
 * 
 * @param state The cluster state
 * @return String representation of the state
 */
const char* cluster_state_to_string(cluster_state_t state);

/**
 * Convert node type to string representation
 * 
 * @param type The node type
 * @return String representation of the node type
 */
const char* cluster_node_type_to_string(cluster_node_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_CLUSTER_H */