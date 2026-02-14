#ifndef DISTRIC_CLUSTER_H
#define DISTRIC_CLUSTER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Include dependencies
#include <distric_gossip.h>
#include <distric_raft.h>
#include "distric_cluster/worker_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Cluster State Types
// ============================================================================

/**
 * Cluster coordinator state
 */
typedef enum {
    CLUSTER_STATE_UNKNOWN = 0,
    CLUSTER_STATE_INITIALIZING,
    CLUSTER_STATE_ACTIVE,
    CLUSTER_STATE_DEGRADED,
    CLUSTER_STATE_ERROR,
    CLUSTER_STATE_SHUTDOWN
} cluster_state_t;

/**
 * Cluster coordinator opaque type
 */
typedef struct cluster_coordinator_s cluster_coordinator_t;

/**
 * Cluster configuration
 */
typedef struct {
    uint32_t heartbeat_interval_ms;
    uint32_t election_timeout_ms;
    uint32_t max_workers;
    bool enable_auto_scaling;
} cluster_config_t;

// ============================================================================
// Cluster Coordinator API
// ============================================================================

/**
 * Create a cluster coordinator
 *
 * @param config Cluster configuration
 * @param out_coordinator Output parameter for created coordinator
 * @return DISTRIC_ERR_INVALID_ARG if out_coordinator is NULL, 0 on success
 */
distric_err_t cluster_coordinator_create(
    const cluster_config_t* config,
    cluster_coordinator_t** out_coordinator
);

/**
 * Destroy a cluster coordinator
 *
 * @param coordinator Coordinator to destroy
 */
void cluster_coordinator_destroy(cluster_coordinator_t* coordinator);

/**
 * Start the cluster coordinator
 *
 * @param coordinator Cluster coordinator
 * @return 0 on success, error code otherwise
 */
distric_err_t cluster_coordinator_start(cluster_coordinator_t* coordinator);

/**
 * Stop the cluster coordinator
 *
 * @param coordinator Cluster coordinator
 * @return 0 on success, error code otherwise
 */
distric_err_t cluster_coordinator_stop(cluster_coordinator_t* coordinator);

/**
 * Get the current cluster state
 *
 * @param coordinator Cluster coordinator
 * @param out_state Output parameter for cluster state
 * @return DISTRIC_ERR_INVALID_ARG if parameters are NULL, 0 on success
 */
distric_err_t cluster_coordinator_get_state(
    const cluster_coordinator_t* coordinator,
    cluster_state_t* out_state
);

/**
 * Check if this coordinator is the cluster leader
 *
 * @param coordinator Cluster coordinator
 * @param out_is_leader Output parameter for leader status
 * @return DISTRIC_ERR_INVALID_ARG if parameters are NULL, 0 on success
 */
distric_err_t cluster_coordinator_is_leader(
    const cluster_coordinator_t* coordinator,
    bool* out_is_leader
);

/**
 * Get the current leader node ID
 *
 * @param coordinator Cluster coordinator
 * @param out_leader_id Output buffer for leader ID (must be >= 256 bytes)
 * @param leader_id_size Size of output buffer
 * @return 0 on success, error code otherwise
 */
distric_err_t cluster_coordinator_get_leader(
    const cluster_coordinator_t* coordinator,
    char* out_leader_id,
    size_t leader_id_size
);

/**
 * Get cluster membership count
 *
 * @param coordinator Cluster coordinator
 * @param out_count Output parameter for node count
 * @return 0 on success, error code otherwise
 */
distric_err_t cluster_coordinator_get_member_count(
    const cluster_coordinator_t* coordinator,
    uint32_t* out_count
);

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Get version information
 *
 * @return Version string
 */
const char* distric_cluster_version(void);

/**
 * Convert cluster state to string
 *
 * @param state Cluster state enum
 * @return String representation
 */
const char* cluster_state_to_string(cluster_state_t state);

#ifdef __cplusplus
}
#endif

#endif // DISTRIC_CLUSTER_H