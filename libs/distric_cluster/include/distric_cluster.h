/**
 * @file distric_cluster.h
 * @brief DistriC Cluster Management - Unified Raft + Gossip Integration
 * 
 * This is the main header for the cluster management layer, which integrates:
 * - Raft consensus for coordination nodes
 * - Gossip protocol for failure detection across all nodes
 * - Worker pool management based on Gossip health data
 * 
 * Architecture:
 * - Coordination nodes: Run both Raft and Gossip
 * - Worker nodes: Run only Gossip
 * - Leader coordinator: Distributes tasks to healthy workers
 * 
 * @section usage Usage Example
 * 
 * @code
 * // Coordination Node
 * cluster_config_t config = {
 *     .node_id = "coord-1",
 *     .node_type = CLUSTER_NODE_COORDINATOR,
 *     .bind_address = "0.0.0.0",
 *     .gossip_port = 7946,
 *     .raft_peers = {"coord-1:8300", "coord-2:8300", "coord-3:8300"},
 *     .raft_peer_count = 3
 * };
 * 
 * cluster_coordinator_t* coord;
 * cluster_coordinator_create(&config, &coord);
 * cluster_coordinator_start(coord);
 * 
 * // Worker Node
 * cluster_config_t worker_config = {
 *     .node_id = "worker-1",
 *     .node_type = CLUSTER_NODE_WORKER,
 *     .bind_address = "0.0.0.0",
 *     .gossip_port = 7946,
 *     .gossip_seeds = {"coord-1:7946", "coord-2:7946"},
 *     .gossip_seed_count = 2
 * };
 * 
 * cluster_coordinator_t* worker;
 * cluster_coordinator_create(&worker_config, &worker);
 * cluster_coordinator_start(worker);
 * @endcode
 * 
 * @version 1.0
 * @date 2026-02-12
 */

#ifndef DISTRIC_CLUSTER_H
#define DISTRIC_CLUSTER_H

#include <distric_obs.h>
#include <distric_raft.h>
#include <distric_gossip.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * FORWARD DECLARATIONS
 * ========================================================================= */

typedef struct cluster_coordinator_s cluster_coordinator_t;
typedef struct cluster_config_s cluster_config_t;
typedef struct cluster_node_s cluster_node_t;

/* ============================================================================
 * ENUMERATIONS
 * ========================================================================= */

/**
 * @brief Node type in the cluster
 */
typedef enum {
    CLUSTER_NODE_COORDINATOR = 0,  /**< Coordination node (Raft + Gossip) */
    CLUSTER_NODE_WORKER           /**< Worker node (Gossip only) */
} cluster_node_type_t;

/**
 * @brief Cluster coordinator state
 */
typedef enum {
    CLUSTER_STATE_STOPPED = 0,    /**< Not started */
    CLUSTER_STATE_STARTING,       /**< Initialization in progress */
    CLUSTER_STATE_RUNNING,        /**< Fully operational */
    CLUSTER_STATE_STOPPING,       /**< Graceful shutdown in progress */
    CLUSTER_STATE_ERROR           /**< Error state */
} cluster_state_t;

/**
 * @brief Raft leadership status
 */
typedef enum {
    CLUSTER_LEADER_STATUS_FOLLOWER = 0,  /**< Not the leader */
    CLUSTER_LEADER_STATUS_CANDIDATE,     /**< Election in progress */
    CLUSTER_LEADER_STATUS_LEADER         /**< This node is the leader */
} cluster_leader_status_t;

/* ============================================================================
 * CONFIGURATION
 * ========================================================================= */

/**
 * @brief Cluster configuration
 */
struct cluster_config_s {
    /* Node identification */
    char node_id[64];                    /**< Unique node identifier */
    cluster_node_type_t node_type;       /**< Coordinator or worker */
    
    /* Network configuration */
    char bind_address[256];              /**< IP address to bind to */
    uint16_t gossip_port;                /**< UDP port for gossip */
    uint16_t raft_port;                  /**< TCP port for Raft (coordinators only) */
    
    /* Gossip configuration */
    char gossip_seeds[16][256];          /**< Seed nodes for gossip */
    size_t gossip_seed_count;            /**< Number of seed nodes */
    uint64_t gossip_interval_ms;         /**< Gossip protocol period (default: 1000) */
    uint64_t gossip_suspect_timeout_ms;  /**< Suspicion timeout (default: 5000) */
    
    /* Raft configuration (coordinators only) */
    char raft_peers[16][256];            /**< Raft peer addresses (host:port) */
    size_t raft_peer_count;              /**< Number of Raft peers */
    uint64_t raft_election_timeout_min_ms;  /**< Min election timeout (default: 150) */
    uint64_t raft_election_timeout_max_ms;  /**< Max election timeout (default: 300) */
    uint64_t raft_heartbeat_interval_ms;    /**< Heartbeat interval (default: 50) */
    
    /* Worker configuration (workers only) */
    uint32_t max_concurrent_tasks;       /**< Max tasks this worker can handle */
    
    /* Observability */
    metrics_registry_t* metrics;         /**< Metrics registry (optional) */
    logger_t* logger;                    /**< Logger (optional) */
    tracer_t* tracer;                    /**< Tracer (optional) */
    
    /* Storage */
    char storage_path[512];              /**< Path for persistent state */
};

/* ============================================================================
 * CLUSTER COORDINATOR API
 * ========================================================================= */

/**
 * @brief Create a cluster coordinator
 * 
 * Initializes both Raft (if coordinator) and Gossip subsystems.
 * Does not start the protocols - call cluster_coordinator_start().
 * 
 * @param config Configuration
 * @param coord_out Output coordinator handle
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t cluster_coordinator_create(
    const cluster_config_t* config,
    cluster_coordinator_t** coord_out
);

/**
 * @brief Start the cluster coordinator
 * 
 * Starts Raft (if coordinator) and Gossip protocols.
 * For coordinators, begins leader election.
 * For workers, joins the gossip network.
 * 
 * @param coord Cluster coordinator
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Must be called from main thread
 */
distric_err_t cluster_coordinator_start(cluster_coordinator_t* coord);

/**
 * @brief Stop the cluster coordinator
 * 
 * Gracefully shuts down all protocols.
 * For coordinators, steps down from leadership if leader.
 * For workers, announces departure via gossip.
 * 
 * @param coord Cluster coordinator
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread (blocks until stopped)
 */
distric_err_t cluster_coordinator_stop(cluster_coordinator_t* coord);

/**
 * @brief Destroy the cluster coordinator
 * 
 * Frees all resources. Must call cluster_coordinator_stop() first.
 * 
 * @param coord Cluster coordinator
 * 
 * Thread Safety: Must be called after coordinator is stopped
 */
void cluster_coordinator_destroy(cluster_coordinator_t* coord);

/* ============================================================================
 * CLUSTER STATE QUERIES
 * ========================================================================= */

/**
 * @brief Get current cluster state
 * 
 * @param coord Cluster coordinator
 * @param state_out Output state
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t cluster_coordinator_get_state(
    const cluster_coordinator_t* coord,
    cluster_state_t* state_out
);

/**
 * @brief Check if this node is the Raft leader
 * 
 * Only meaningful for coordinator nodes.
 * 
 * @param coord Cluster coordinator
 * @param is_leader_out Output boolean
 * @return DISTRIC_OK on success, DISTRIC_ERR_INVALID_STATE if not coordinator
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t cluster_coordinator_is_leader(
    const cluster_coordinator_t* coord,
    bool* is_leader_out
);

/**
 * @brief Get current Raft leader ID
 * 
 * @param coord Cluster coordinator
 * @param leader_id_out Output buffer (min 64 bytes)
 * @param buffer_size Size of output buffer
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t cluster_coordinator_get_leader_id(
    const cluster_coordinator_t* coord,
    char* leader_id_out,
    size_t buffer_size
);

/**
 * @brief Get count of alive nodes by type
 * 
 * @param coord Cluster coordinator
 * @param node_type Filter by type (or -1 for all)
 * @param count_out Output count
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t cluster_coordinator_get_node_count(
    const cluster_coordinator_t* coord,
    cluster_node_type_t node_type,
    size_t* count_out
);

/**
 * @brief Get list of alive worker nodes
 * 
 * Caller must free the returned array.
 * 
 * @param coord Cluster coordinator
 * @param workers_out Output array of worker info (allocated by function)
 * @param count_out Number of workers returned
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t cluster_coordinator_get_alive_workers(
    const cluster_coordinator_t* coord,
    cluster_node_t** workers_out,
    size_t* count_out
);

/* ============================================================================
 * WORKER MANAGEMENT (Leader Only)
 * ========================================================================= */

/**
 * @brief Get an available worker for task assignment
 * 
 * Only works if this node is the Raft leader.
 * Uses load balancing strategy to select best worker.
 * 
 * @param coord Cluster coordinator
 * @param worker_id_out Output buffer for worker ID (min 64 bytes)
 * @param buffer_size Size of output buffer
 * @return DISTRIC_OK on success, DISTRIC_ERR_NOT_FOUND if no workers available
 * 
 * Thread Safety: Safe to call from any thread (leader only)
 */
distric_err_t cluster_coordinator_select_worker(
    cluster_coordinator_t* coord,
    char* worker_id_out,
    size_t buffer_size
);

/**
 * @brief Update worker load information
 * 
 * Called when task is assigned or completed to track worker load.
 * 
 * @param coord Cluster coordinator
 * @param worker_id Worker node ID
 * @param load_delta Change in load (+1 for assign, -1 for complete)
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t cluster_coordinator_update_worker_load(
    cluster_coordinator_t* coord,
    const char* worker_id,
    int32_t load_delta
);

/* ============================================================================
 * CALLBACKS
 * ========================================================================= */

/**
 * @brief Callback when this node becomes Raft leader
 * 
 * @param coord Cluster coordinator
 * @param user_data User-provided context
 */
typedef void (*cluster_on_became_leader_fn)(
    cluster_coordinator_t* coord,
    void* user_data
);

/**
 * @brief Callback when this node loses Raft leadership
 * 
 * @param coord Cluster coordinator
 * @param user_data User-provided context
 */
typedef void (*cluster_on_lost_leadership_fn)(
    cluster_coordinator_t* coord,
    void* user_data
);

/**
 * @brief Callback when a worker node joins
 * 
 * @param coord Cluster coordinator
 * @param worker Worker node info
 * @param user_data User-provided context
 */
typedef void (*cluster_on_worker_joined_fn)(
    cluster_coordinator_t* coord,
    const cluster_node_t* worker,
    void* user_data
);

/**
 * @brief Callback when a worker node fails
 * 
 * @param coord Cluster coordinator
 * @param worker Worker node info
 * @param user_data User-provided context
 */
typedef void (*cluster_on_worker_failed_fn)(
    cluster_coordinator_t* coord,
    const cluster_node_t* worker,
    void* user_data
);

/**
 * @brief Register callback for leadership changes
 * 
 * @param coord Cluster coordinator
 * @param on_became_leader Callback when becoming leader
 * @param on_lost_leadership Callback when losing leadership
 * @param user_data User context passed to callbacks
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t cluster_coordinator_set_leadership_callbacks(
    cluster_coordinator_t* coord,
    cluster_on_became_leader_fn on_became_leader,
    cluster_on_lost_leadership_fn on_lost_leadership,
    void* user_data
);

/**
 * @brief Register callbacks for worker events
 * 
 * @param coord Cluster coordinator
 * @param on_worker_joined Callback when worker joins
 * @param on_worker_failed Callback when worker fails
 * @param user_data User context passed to callbacks
 * @return DISTRIC_OK on success, error code otherwise
 */
distric_err_t cluster_coordinator_set_worker_callbacks(
    cluster_coordinator_t* coord,
    cluster_on_worker_joined_fn on_worker_joined,
    cluster_on_worker_failed_fn on_worker_failed,
    void* user_data
);

/* ============================================================================
 * CLUSTER NODE INFO
 * ========================================================================= */

/**
 * @brief Information about a node in the cluster
 */
struct cluster_node_s {
    char node_id[64];                     /**< Unique node identifier */
    char address[256];                    /**< Network address */
    uint16_t gossip_port;                 /**< Gossip port */
    uint16_t raft_port;                   /**< Raft port (coordinators only) */
    cluster_node_type_t node_type;        /**< Coordinator or worker */
    gossip_node_status_t gossip_status;   /**< Health status from gossip */
    uint64_t last_seen_ms;                /**< Timestamp of last gossip update */
    uint64_t load;                        /**< Current load (workers only) */
    uint32_t max_concurrent_tasks;        /**< Capacity (workers only) */
};

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

/**
 * @brief Get string representation of cluster state
 * 
 * @param state Cluster state
 * @return String (do not free)
 */
const char* cluster_state_to_string(cluster_state_t state);

/**
 * @brief Get string representation of node type
 * 
 * @param type Node type
 * @return String (do not free)
 */
const char* cluster_node_type_to_string(cluster_node_type_t type);

/**
 * @brief Get library version
 * 
 * @return Version string (e.g., "1.0.0")
 */
const char* distric_cluster_version(void);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_CLUSTER_H */