/**
 * @file worker_pool.h
 * @brief Worker Pool Management for DistriC Cluster
 * 
 * Manages a pool of worker nodes, tracking their health status and
 * capacity for task distribution.
 */

#ifndef DISTRIC_CLUSTER_WORKER_POOL_H
#define DISTRIC_CLUSTER_WORKER_POOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "distric_obs.h"
#include "distric_gossip.h"  /* Import gossip types instead of redefining them */

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct worker_pool_s worker_pool_t;

/* Worker selection strategy */
typedef enum {
    WORKER_SELECT_ROUND_ROBIN = 0,
    WORKER_SELECT_LEAST_LOADED = 1,
    WORKER_SELECT_LEAST_UTILIZED = 2,
    WORKER_SELECT_RANDOM = 3
} worker_selection_strategy_t;

/* Worker information structure */
typedef struct {
    char worker_id[64];
    char address[256];
    uint16_t port;
    
    gossip_node_status_t status;  /* Use type from distric_gossip.h */
    uint32_t incarnation;
    
    uint64_t joined_at_ms;
    uint64_t last_seen_ms;
    uint64_t last_task_assigned_ms;
    
    uint32_t max_concurrent_tasks;
    uint32_t current_task_count;
    uint32_t total_tasks_completed;
    
    double load_metric;
    double utilization;
} worker_info_t;

/* Worker pool capacity statistics */
typedef struct {
    size_t total_workers;
    size_t alive_workers;
    uint32_t total_capacity;
    uint32_t total_utilized;
    double utilization_rate;
    double average_load;
} worker_pool_capacity_stats_t;

/**
 * Create a new worker pool
 * 
 * @param metrics Optional metrics registry
 * @param logger Optional logger
 * @return New worker pool instance or NULL on error
 */
worker_pool_t* worker_pool_create(
    metrics_registry_t* metrics,
    logger_t* logger
);

/**
 * Destroy a worker pool
 * 
 * @param pool Worker pool to destroy
 */
void worker_pool_destroy(worker_pool_t* pool);

/**
 * Update worker pool from gossip protocol information
 * 
 * @param pool Worker pool
 * @param node Gossip node information
 * @return 0 on success, -1 on error
 */
int worker_pool_update_from_gossip(
    worker_pool_t* pool,
    const gossip_node_info_t* node
);

/**
 * Mark a worker as failed
 * 
 * @param pool Worker pool
 * @param worker_id Worker identifier
 * @return 0 on success, -1 on error
 */
int worker_pool_mark_failed(
    worker_pool_t* pool,
    const char* worker_id
);

/**
 * Select a worker based on the configured strategy
 * 
 * @param pool Worker pool
 * @param strategy Selection strategy to use
 * @param worker_out Output pointer for selected worker (read-only)
 * @return 0 on success, -1 if no worker available
 */
int worker_pool_select_worker(
    worker_pool_t* pool,
    worker_selection_strategy_t strategy,
    const worker_info_t** worker_out
);

/**
 * Update task count for a specific worker
 * 
 * @param pool Worker pool
 * @param worker_id Worker identifier
 * @param task_delta Change in task count (positive or negative)
 * @return 0 on success, -1 on error
 */
int worker_pool_update_task_count(
    worker_pool_t* pool,
    const char* worker_id,
    int32_t task_delta
);

/**
 * Get all workers matching a specific status
 * 
 * @param pool Worker pool
 * @param status Status filter
 * @param workers_out Output array of worker pointers (caller must free)
 * @param count_out Number of workers returned
 * @return 0 on success, -1 on error
 */
int worker_pool_get_workers_by_status(
    worker_pool_t* pool,
    gossip_node_status_t status,
    worker_info_t** workers_out,
    size_t* count_out
);

/**
 * Get information about a specific worker
 * 
 * @param pool Worker pool
 * @param worker_id Worker identifier
 * @param worker_out Output structure to fill
 * @return 0 on success, -1 if worker not found
 */
int worker_pool_get_worker_info(
    worker_pool_t* pool,
    const char* worker_id,
    worker_info_t* worker_out
);

/**
 * Get capacity statistics for the worker pool
 * 
 * @param pool Worker pool
 * @param stats_out Output structure for statistics
 * @return 0 on success, -1 on error
 */
int worker_pool_get_capacity_stats(
    worker_pool_t* pool,
    worker_pool_capacity_stats_t* stats_out
);

/**
 * Set the worker selection strategy
 * 
 * @param pool Worker pool
 * @param strategy Strategy to use
 * @return 0 on success, -1 on error
 */
int worker_pool_set_strategy(
    worker_pool_t* pool,
    worker_selection_strategy_t strategy
);

/**
 * Get the current worker selection strategy
 * 
 * @param pool Worker pool
 * @param strategy Output pointer for current strategy
 * @return 0 on success, -1 on error
 */
int worker_pool_get_strategy(
    worker_pool_t* pool,
    worker_selection_strategy_t* strategy
);

/* ====================================================================
 * Utility Functions
 * ==================================================================== */

/**
 * Convert worker selection strategy enum to string
 * 
 * @param strategy Worker selection strategy
 * @return String representation of the strategy
 */
const char* worker_selection_strategy_to_string(
    worker_selection_strategy_t strategy
);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_CLUSTER_WORKER_POOL_H */