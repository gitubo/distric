/**
 * @file worker_pool.h
 * @brief Worker Pool Management with Gossip Integration
 * 
 * Manages a pool of worker nodes, tracking their:
 * - Health status (from Gossip)
 * - Current load
 * - Capacity
 * 
 * Provides load balancing strategies for task assignment.
 * 
 * @version 1.0
 * @date 2026-02-12
 */

#ifndef DISTRIC_CLUSTER_WORKER_POOL_H
#define DISTRIC_CLUSTER_WORKER_POOL_H

#include <distric_obs.h>
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

typedef struct worker_pool_s worker_pool_t;
typedef struct worker_info_s worker_info_t;

/* ============================================================================
 * WORKER INFO
 * ========================================================================= */

/**
 * @brief Information about a worker node
 */
struct worker_info_s {
    char worker_id[64];                   /**< Unique worker identifier */
    char address[256];                    /**< Network address */
    uint16_t port;                        /**< Worker service port */
    
    /* Capacity */
    uint32_t max_concurrent_tasks;        /**< Maximum concurrent tasks */
    uint32_t current_task_count;          /**< Current number of tasks */
    uint64_t total_tasks_completed;       /**< Lifetime task counter */
    
    /* Health (from Gossip) */
    gossip_node_status_t status;          /**< Health status */
    uint64_t last_seen_ms;                /**< Last gossip update timestamp */
    uint64_t incarnation;                 /**< Gossip incarnation number */
    
    /* Load metrics */
    uint64_t load_metric;                 /**< Load value from gossip */
    double utilization;                   /**< current_tasks / max_tasks */
    
    /* Timestamps */
    uint64_t joined_at_ms;                /**< When worker joined */
    uint64_t last_task_assigned_ms;       /**< Last task assignment time */
};

/* ============================================================================
 * LOAD BALANCING STRATEGIES
 * ========================================================================= */

/**
 * @brief Load balancing strategy for worker selection
 */
typedef enum {
    WORKER_SELECT_ROUND_ROBIN = 0,   /**< Simple round-robin */
    WORKER_SELECT_LEAST_LOADED,      /**< Lowest current task count */
    WORKER_SELECT_LEAST_UTILIZED,    /**< Lowest utilization percentage */
    WORKER_SELECT_RANDOM             /**< Random selection */
} worker_selection_strategy_t;

/* ============================================================================
 * WORKER POOL API
 * ========================================================================= */

/**
 * @brief Create a worker pool
 * 
 * @param metrics Metrics registry (optional)
 * @param logger Logger (optional)
 * @param pool_out Output pool handle
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t worker_pool_create(
    metrics_registry_t* metrics,
    logger_t* logger,
    worker_pool_t** pool_out
);

/**
 * @brief Destroy a worker pool
 * 
 * Frees all resources.
 * 
 * @param pool Worker pool
 * 
 * Thread Safety: Must be called when no other threads are using the pool
 */
void worker_pool_destroy(worker_pool_t* pool);

/**
 * @brief Update worker info from gossip
 * 
 * Called when gossip protocol receives node updates.
 * Adds new workers or updates existing ones.
 * 
 * @param pool Worker pool
 * @param node Gossip node info
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from gossip callback thread
 */
distric_err_t worker_pool_update_from_gossip(
    worker_pool_t* pool,
    const gossip_node_info_t* node
);

/**
 * @brief Mark a worker as failed
 * 
 * Called when gossip detects worker failure.
 * 
 * @param pool Worker pool
 * @param worker_id Worker node ID
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from gossip callback thread
 */
distric_err_t worker_pool_mark_failed(
    worker_pool_t* pool,
    const char* worker_id
);

/**
 * @brief Get an available worker for task assignment
 * 
 * Selects a worker based on the configured strategy.
 * Only returns healthy workers with available capacity.
 * 
 * @param pool Worker pool
 * @param strategy Selection strategy
 * @param worker_out Output worker info (pointer valid until next pool operation)
 * @return DISTRIC_OK on success, DISTRIC_ERR_NOT_FOUND if no workers available
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t worker_pool_get_available(
    worker_pool_t* pool,
    worker_selection_strategy_t strategy,
    const worker_info_t** worker_out
);

/**
 * @brief Update worker task count
 * 
 * Increment (+1) when assigning task, decrement (-1) when task completes.
 * 
 * @param pool Worker pool
 * @param worker_id Worker node ID
 * @param task_delta Change in task count (+1 or -1)
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t worker_pool_update_task_count(
    worker_pool_t* pool,
    const char* worker_id,
    int32_t task_delta
);

/**
 * @brief Get count of workers by status
 * 
 * @param pool Worker pool
 * @param status Filter by status (or -1 for all)
 * @param count_out Output count
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t worker_pool_get_count(
    const worker_pool_t* pool,
    gossip_node_status_t status,
    size_t* count_out
);

/**
 * @brief Get list of all workers
 * 
 * Caller must free the returned array.
 * 
 * @param pool Worker pool
 * @param workers_out Output array (allocated by function)
 * @param count_out Number of workers
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t worker_pool_get_all_workers(
    const worker_pool_t* pool,
    worker_info_t** workers_out,
    size_t* count_out
);

/**
 * @brief Get worker info by ID
 * 
 * @param pool Worker pool
 * @param worker_id Worker node ID
 * @param worker_out Output worker info
 * @return DISTRIC_OK on success, DISTRIC_ERR_NOT_FOUND if not found
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t worker_pool_get_worker(
    const worker_pool_t* pool,
    const char* worker_id,
    worker_info_t* worker_out
);

/**
 * @brief Get total capacity across all alive workers
 * 
 * @param pool Worker pool
 * @param total_capacity_out Total max concurrent tasks
 * @param total_utilized_out Total current tasks
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t worker_pool_get_capacity_stats(
    const worker_pool_t* pool,
    uint64_t* total_capacity_out,
    uint64_t* total_utilized_out
);

/**
 * @brief Set selection strategy
 * 
 * @param pool Worker pool
 * @param strategy New strategy
 * @return DISTRIC_OK on success, error code otherwise
 * 
 * Thread Safety: Safe to call from any thread
 */
distric_err_t worker_pool_set_strategy(
    worker_pool_t* pool,
    worker_selection_strategy_t strategy
);

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================= */

/**
 * @brief Get string representation of selection strategy
 * 
 * @param strategy Strategy enum
 * @return String (do not free)
 */
const char* worker_selection_strategy_to_string(
    worker_selection_strategy_t strategy
);

#ifdef __cplusplus
}
#endif

#endif /* DISTRIC_CLUSTER_WORKER_POOL_H */